#!/usr/bin/env python3
"""Prepare an unselected 100us-idle indoor waveform candidate, never a runtime profile.

Writes only to an explicitly selected verification/candidates directory. It
does not generate application headers, copy approvals, access hardware, or
change the selected 31us-chirp waveform. New timing constants still need a
joint PS/PL/config qualification before the candidate can be used.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import math
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LIGHT_SPEED = 299792458.0

def make_candidate(source: str) -> tuple[str, dict]:
    lines = source.splitlines()
    commands = [line.split() for line in lines if line.strip() and not line.lstrip().startswith('%')]
    def one(verb):
        matches = [x for x in commands if x[0] == verb]
        if len(matches) != 1:
            raise ValueError('Expected exactly one ' + verb)
        return matches[0]
    profile = one('profileCfg')
    frame = one('frameCfg')
    if profile != 'profileCfg 0 60 7 3 24 0 0 166 1 256 12500 0 0 158'.split():
        raise ValueError('Candidate generation requires the exact approved7us source waveform')
    if one('channelCfg') != 'channelCfg 15 7 0'.split():
        raise ValueError('Expected3TX4RX geometry')
    if len(frame) != 8 or frame[1:4] != ['0', '2', '32'] or frame[5:] != ['100', '1', '0'] or int(frame[4]) not in (0, 8, 16, 64):
        raise ValueError('Unexpected finite/stream frame geometry')
    chirps = [x for x in commands if x[0] == 'chirpCfg']
    wanted = [f'chirpCfg {i} {i} 0 0 0 0 0 {mask}'.split() for i, mask in enumerate((1, 4, 2))]
    if chirps != wanted or one('lvdsStreamCfg') != 'lvdsStreamCfg -1 1 1 0'.split():
        raise ValueError('Expected TX0/TX2/TX1 order and HSI/ADC output')
    velocity = [x for x in commands if x[:3] == ['cfarFovCfg', '-1', '1']]
    if velocity != ['cfarFovCfg -1 1 -12.9 12.9'.split()]:
        raise ValueError('Unexpected source velocity selection')
    samples, loops, tx, rx = 256, 32, 3, 4
    idle_us, ramp_us, adc_start_us = 100.0, 24.0, 3.0
    chirp_us = idle_us+ramp_us
    sample_rate_hz = 12_500_000.0
    slope_hz_per_s = 166e12
    # Same sampled-band center convention as the current spatial implementation.
    center_hz = 60e9+slope_hz_per_s*(adc_start_us*1e-6+samples/sample_rate_hz/2)
    cycle_s = tx*chirp_us*1e-6
    bin_mps = LIGHT_SPEED/(2*center_hz*loops*cycle_s)
    max_mps = LIGHT_SPEED/(4*center_hz*cycle_s)
    safe_bound = math.floor(max_mps*10)/10
    changes = {' '.join(profile): 'profileCfg 0 60 100 3 24 0 0 166 1 256 12500 0 0 158',
               'cfarFovCfg -1 1 -12.9 12.9': f'cfarFovCfg -1 1 -{safe_bound:.1f} {safe_bound:.1f}'}
    candidate = ['% UNSELECTED CANDIDATE / NOT_RUNTIME_READY / NO NEW BOARD EVIDENCE',
                 '% 100us idle requires joint geometry/TDM/velocity/calibration qualification.']
    for line in lines:
        if not line.strip() or line.lstrip().startswith('%'):
            continue
        normalized = ' '.join(line.split())
        candidate.append(changes.get(normalized, line))
    text = '\n'.join(candidate)+'\n'
    metadata = {
        'schema': 'M2_SLOW_WAVEFORM_CANDIDATE_V1', 'selected': False,
        'status': 'NOT_RUNTIME_READY', 'hardware_executed': False,
        'source_cfg_sha256': hashlib.sha256(source.encode()).hexdigest(),
        'candidate_cfg_sha256': hashlib.sha256(text.encode()).hexdigest(),
        'tx_slot_order': [0, 2, 1], 'tx': tx, 'rx': rx, 'samples': samples,
        'loops': loops, 'chirps_per_frame': loops*tx,
        'hsi_record_bytes': 4160, 'frame_bytes': 399360,
        'frame_count': int(frame[4]), 'nominal_frame_period_ms': 100,
        'nominal_period_is_not_measured_time': True,
        'idle_us': idle_us, 'ramp_us': ramp_us, 'chirp_period_us': chirp_us,
        'same_tx_period_us': cycle_s*1e6, 'coherent_observation_span_us': loops*cycle_s*1e6,
        'tdm_slot_offsets_us': [0.0, chirp_us, 2*chirp_us],
        'sampled_band_center_hz': center_hz, 'wavelength_m': LIGHT_SPEED/center_hz,
        'theoretical_doppler_bin_mps': bin_mps,
        'theoretical_unambiguous_speed_abs_mps': max_mps,
        'radar_velocity_filter_abs_mps': safe_bound,
        'physical_performance_measured': False,
        'runtime_header_generated': False,
        'required_before_selection': [
            'Joint waveform geometry generator and identity admission support',
            'PS velocity scale and TDM coefficients124us instead of31us',
            'Calibration identity bound to candidate waveform and installation',
            'Runtime/profile/metadata tests with exact399360-byte frames',
            'Separate real acquisition, stop/recovery and known-motion checks'],
        'does_not_claim': ['Velocity measurement accuracy', 'Respiration or life detection', 'Improved physical target accuracy']}
    return text, metadata

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--source', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    args = ap.parse_args()
    out = args.output.resolve()
    verification = (ROOT/'verification').resolve()
    if not out.is_relative_to(verification) or 'candidates' not in out.relative_to(verification).parts:
        raise SystemExit('Output must be an isolated verification/.../candidates/... directory')
    source_bytes = args.source.read_bytes()
    text, metadata = make_candidate(source_bytes.decode('utf-8'))
    metadata['source_cfg_sha256'] = hashlib.sha256(source_bytes).hexdigest()
    metadata['source_path'] = str(args.source.resolve())
    out.mkdir(parents=True, exist_ok=True)
    cfg = out/'radar_3tx_slow_idle100_UNSELECTED.cfg'
    cfg.write_bytes(text.encode('utf-8'))
    (out/'candidate.json').write_text(json.dumps(metadata, indent=2)+'\n', encoding='utf-8')
    (out/'README.md').write_text(
        '# Unselected slow indoor waveform candidate\n\n'
        'NOT_RUNTIME_READY. These files are preparation only; no runtime header, approval, selected profile or hardware state was changed.\n\n'
        'The candidate keeps3TX/4RX,256samples,32loops and399360bytes/frame, changes idle7us to100us, '
        'and limits the radar-side velocity filter to±3.2m/s. The nominal100ms frame period is not a measurement.\n\n'
        'Current production spatial code uses31us chirp timing. This candidate needs124us chirps and '
        'slot offsets[0,124,248]us. Do not send it while using the old processing constants. '
        'Joint profile/PS timing integration and separate board validation are required. '
        'The existing waveform remains selected.\n', encoding='utf-8')
    print(json.dumps(metadata, indent=2))
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
