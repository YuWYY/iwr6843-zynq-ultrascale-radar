#!/usr/bin/env python3
"""Independent NumPy reference for the fixed single-HSI-frame range processor.

This reads a saved capture; it does not acquire data or prove calibrated range.
The cfg is hash-bound because FFT bin spacing depends on its sample rate/slope.
"""
from __future__ import annotations
import argparse
import csv
import hashlib
import json
from pathlib import Path
import sys
import numpy as np
import decode_adc

CFG_SHA256 = '895994e473a720d80a7550a947542ff73aa52beefcb1418f33e0b1ec7c7f1ee5'
THREE_TX_CFG_SHA256 = '8eb44d742c1e2d00a314a4d8dcbef45732d359d4d4de2d153b57234948904566'
BIN_M = 299792458.0 * 12500000.0 / (2.0 * 166000000000000.0 * 256.0)


def analyze(raw: bytes, cfg: bytes) -> tuple[np.ndarray, dict]:
    cfg_hash = hashlib.sha256(cfg).hexdigest()
    if cfg_hash not in (CFG_SHA256, THREE_TX_CFG_SHA256):
        raise ValueError('range processor requires the frozen single-frame HSI cfg; scale must not be inferred from length')
    geometry = decode_adc.parse_cfg(cfg, 'hsi')
    iq_bytes, adc_report = decode_adc.decode(raw, geometry)
    codes = np.frombuffer(iq_bytes, dtype='<i2').reshape(geometry.chirps, 4, 256, 2)
    x = codes[..., 0].astype(np.float64) + 1j * codes[..., 1].astype(np.float64)
    window = np.hanning(256)
    spectrum = np.fft.fft((x - x.mean(axis=-1, keepdims=True)) * window, axis=-1)
    power = np.mean(np.abs(spectrum) ** 2, axis=(0, 1)) / (32768.0 * window.sum()) ** 2
    dbfs = 10.0 * np.log10(np.maximum(power, 1e-16))
    peak = 6 + int(np.argmax(dbfs[6:205]))
    strongest = int(np.argmax(dbfs))
    tx_records = [0, 0, 0]
    tx_power_dbfs = [-160.0, -160.0, -160.0]
    for slot, tx in enumerate(geometry.tx_sequence):
        samples = x[slot::len(geometry.tx_sequence)]
        tx_records[tx] = len(samples)
        tx_power_dbfs[tx] = float(10 * np.log10(max(float(np.mean(np.abs(samples)**2)) / 32768.0**2, 1e-16)))
    summary = {
        'schema': 1,
        'status': 'OFFLINE_NUMPY_RANGE_ANALYSIS_PASS',
        'raw_sha256': hashlib.sha256(raw).hexdigest(),
        'cfg_sha256': cfg_hash,
        'raw_bytes': len(raw), 'hsi_records': len(adc_report['hsi_records']),
        'algorithm': f'complex256 forward FFT; per chirp/RX mean subtraction; symmetric Hann; linear power average of{geometry.chirps}x4 transforms',
        'tx_sequence': list(geometry.tx_sequence), 'tx_records': tx_records,
        'tx_power_dbfs': tx_power_dbfs,
        'tx_power_basis': 'physical TX index; mean raw ADC I^2+Q^2 including DC / 32768^2, then log10',
        'scale': '10log10(mean(|FFT|^2)/(32768*sum(Hann))^2), floor -160dBFS',
        'sample_rate_hz_nominal': 12500000.0, 'chirp_slope_hz_per_s_nominal': 166000000000000.0,
        'bin_spacing_m_nominal': BIN_M,
        'display_range_m': [0.25, 9.0], 'display_bins_inclusive': [6, 204],
        'strongest_display_bin': peak, 'strongest_display_range_m_nominal': peak * BIN_M,
        'strongest_display_dbfs': float(dbfs[peak]),
        'strongest_all_bin': strongest, 'strongest_all_dbfs': float(dbfs[strongest]),
        'median_all_bins_dbfs': float(np.median(dbfs)),
        'adc_min_code': int(codes.min()), 'adc_max_code': int(codes.max()),
        'clipped_components': int(np.count_nonzero((codes == -32768) | (codes == 32767))),
        'power_dbfs': dbfs.tolist(),
        'limits': [
            'Saved input replay; source may be captured or synthetic. No new acquisition is implied.',
            'Peak is strongest spectral return, not CFAR target identification.',
            'dBFS is ADC-code-relative, not dBm, RCS or calibrated RF power.',
            'Nominal bin spacing is not measured distance accuracy or two-target resolution.',
            'Complex1x positive IF uses full256 unshifted bins; do not truncate at128 or conjugate to fit a scene.',
            'No angle, velocity or range-bias calibration.'
        ],
    }
    return dbfs, summary


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', type=Path)
    parser.add_argument('--cfg', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path, help='JSON report; sibling CSV is also written')
    parser.add_argument('--c-csv', type=Path, help='Compare independently computed bins with test_range C CSV')
    args = parser.parse_args()
    try:
        power, result = analyze(args.input.read_bytes(), args.cfg.read_bytes())
        if args.c_csv:
            with args.c_csv.open(newline='', encoding='utf-8') as stream:
                rows = list(csv.DictReader(stream))
            if len(rows) != 256 or [int(row['bin']) for row in rows] != list(range(256)):
                raise ValueError('C comparison must contain all256 ordered bins')
            c_power = np.array([float(row['power_dbfs']) for row in rows])
            c_range = np.array([float(row['range_m']) for row in rows])
            error = float(np.max(np.abs(power - c_power)))
            if not np.all(np.isfinite(c_power)) or error > 0.02:
                raise ValueError(f'C/NumPy spectral mismatch: maxerror={error}dB')
            if float(np.max(np.abs(c_range - np.arange(256) * BIN_M))) > 1e-8:
                raise ValueError('C/NumPy nominal distance scale mismatch')
            result['c_comparison'] = {'status': 'PASS', 'bins': 256, 'max_abs_db_error': error,
                                      'tolerance_db': 0.02, 'c_csv_sha256': hashlib.sha256(args.c_csv.read_bytes()).hexdigest()}
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
        with args.output.with_suffix('.csv').open('w', newline='', encoding='utf-8') as stream:
            out = csv.writer(stream); out.writerow(['bin', 'range_m_nominal', 'power_dbfs'])
            for k, value in enumerate(power): out.writerow([k, k * BIN_M, float(value)])
        compact = {key: result[key] for key in ('status', 'strongest_display_bin', 'strongest_display_range_m_nominal', 'strongest_display_dbfs', 'clipped_components')}
        if 'c_comparison' in result: compact['c_comparison'] = result['c_comparison']
        print(json.dumps(compact, indent=2))
        return 0
    except (OSError, ValueError, decode_adc.DecodeError) as error:
        print(f'RANGE_ANALYSIS_FAILED {error}', file=sys.stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
