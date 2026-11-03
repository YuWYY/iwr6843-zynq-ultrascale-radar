#!/usr/bin/env python3
"""Independent NumPy reference for the fixed M2 3TX spatial preview.

Reads saved RAW16 only. This does not acquire data or establish physical angle
accuracy. Coordinates are sensor-relative; unity calibration is labelled
UNCALIBRATED even when the nominal model gives numerically valid coordinates.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import sys

import numpy as np
import decode_adc

CFG_SHA256 = "8eb44d742c1e2d00a314a4d8dcbef45732d359d4d4de2d153b57234948904566"
ARRAY_ID = "IWR6843ISK_2P5MM_TX021_V1"
C = 299792458.0
FS = 12500000.0
SLOPE = 166000000000000.0
FC = 60e9 + SLOPE * (3e-6 + 256.0 / (2.0 * FS))
WAVELENGTH = C / FC
BIN_M = C * FS / (2.0 * SLOPE * 256.0)
VELOCITY_BIN = WAVELENGTH / (2.0 * 32.0 * 93e-6)
TX_SEQUENCE = (0, 2, 1)
SLOTS = np.repeat([0, 2, 1], 4)
# Physical TX-major order. TI integer coordinates use design spacing 2.5 mm.
POSITIONS = 0.0025 * np.array([
    [0, -1], [-1, -1], [-2, -1], [-3, -1],
    [-2, 0], [-3, 0], [-4, 0], [-5, 0],
    [-4, -1], [-5, -1], [-6, -1], [-7, -1],
], dtype=np.float64)
HORIZONTAL = np.array([0, 1, 2, 3, 8, 9, 10, 11])
AZIMUTHS = np.arange(-60.0, 61.0)
ELEVATIONS = np.arange(-15.0, 16.0)
SIGNED_DOPPLER = np.r_[np.arange(16), np.arange(-16, 0)]
AZ_VALID, EL_VALID, XYZ_VALID = 1, 2, 4
DOPPLER_EDGE, LOW_COHERENCE, ANGLE_EDGE = 8, 16, 32


@dataclass(frozen=True)
class Calibration:
    channel: np.ndarray
    range_bias_m: float = 0.0
    measured: bool = False
    calibration_id: str = "UNCALIBRATED"


def parse_calibration(value: dict | None) -> Calibration:
    if value is None:
        return Calibration(np.ones(12, dtype=np.complex128))
    if not isinstance(value, dict):
        raise ValueError("calibration must be an object")
    required = {"array_id", "cfg_sha256", "calibration_id", "measured", "range_bias_m", "channel"}
    if set(value) != required:
        raise ValueError("calibration requires exactly the documented six fields")
    if value["array_id"] != ARRAY_ID or value["cfg_sha256"] != CFG_SHA256:
        raise ValueError("calibration array/config identity mismatch")
    channel = np.asarray(value["channel"], dtype=np.float64)
    if channel.shape != (12, 2) or not np.all(np.isfinite(channel)):
        raise ValueError("calibration requires 12 finite physical-TX [real,imag] pairs")
    coeff = channel[:, 0] + 1j * channel[:, 1]
    if np.any((coeff.real == 0.0) & (coeff.imag == 0.0)):
        raise ValueError("calibration channel coefficients must be nonzero")
    if isinstance(value["range_bias_m"], bool):
        raise ValueError("range bias must be a finite number")
    bias = float(value["range_bias_m"])
    if not np.isfinite(bias):
        raise ValueError("range bias must be finite")
    if value["measured"] not in (False, True, 0, 1):
        raise ValueError("measured must be a boolean")
    identity = value["calibration_id"]
    if not isinstance(identity, str) or not 1 <= len(identity) <= 47 or not identity.isascii():
        raise ValueError("calibration_id must be 1..47 ASCII characters")
    if bool(value["measured"]) and identity == "UNCALIBRATED":
        raise ValueError("measured calibration requires its own identity")
    return Calibration(coeff, bias, bool(value["measured"]), identity)


def _codes(raw: bytes, cfg: bytes) -> tuple[np.ndarray, dict]:
    if hashlib.sha256(cfg).hexdigest() != CFG_SHA256:
        raise ValueError("spatial analysis requires the frozen 3TX single-frame cfg")
    geometry = decode_adc.parse_cfg(cfg, "hsi")
    if geometry.tx_sequence != TX_SEQUENCE or geometry.frames != 1:
        raise ValueError("spatial analysis requires one frame in TX0/TX2/TX1 order")
    iq, metadata = decode_adc.decode(raw, geometry)
    codes = np.frombuffer(iq, dtype="<i2").reshape(32, 3, 4, 256, 2)
    samples = codes[..., 0].astype(np.float64) + 1j * codes[..., 1].astype(np.float64)
    # Convert slot-major input into physical TX-major complex channels.
    samples = samples[:, [0, 2, 1]].reshape(32, 12, 256)
    return samples, metadata


def range_and_doppler(samples: np.ndarray) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    samples = np.asarray(samples)
    if samples.shape != (32, 12, 256) or not np.all(np.isfinite(samples)):
        raise ValueError("samples require finite complex [32,12,256] physical-TX array")
    wr = np.hanning(256)
    cube = np.fft.fft((samples - samples.mean(axis=2, keepdims=True)) * wr, axis=2)
    cube /= 32768.0 * wr.sum()
    overview = 10.0 * np.log10(np.maximum(np.mean(np.abs(cube) ** 2, axis=(0, 1)), 1e-16))
    # Result axes [range, unsigned Doppler index, physical channel].
    doppler = np.fft.fft(cube * np.hanning(32)[:, None, None], axis=0).transpose(2, 0, 1)
    return cube, doppler, overview


def corrected_channels(doppler: np.ndarray, calibration: Calibration) -> np.ndarray:
    doppler = np.asarray(doppler)
    if doppler.shape != (256, 32, 12) or not np.all(np.isfinite(doppler)):
        raise ValueError("Doppler cube requires finite [256,32,12] array")
    if not isinstance(calibration, Calibration) or calibration.channel.shape != (12,) or not np.all(np.isfinite(calibration.channel)):
        raise ValueError("calibration requires 12 finite physical-channel coefficients")
    phase = np.exp(-2j * np.pi * SIGNED_DOPPLER[:, None] * SLOTS[None, :] / 96.0)
    return doppler * phase[None, :, :] * calibration.channel[None, None, :]


def cfar_candidates(power: np.ndarray) -> tuple[list[tuple[int, int, float, float]], int]:
    """Independent explicit-window CA-CFAR, then deterministic 3x3 NMS."""
    power = np.asarray(power, dtype=np.float64)
    if power.shape != (256, 32) or not np.all(np.isfinite(power)) or np.any(power < 0):
        raise ValueError("CFAR power requires nonnegative finite [256,32]")
    found = []
    threshold = 10.0 ** 1.2
    for r in range(6, 205):
        rr = np.arange(max(6, r - 10), min(205, r + 11))
        for k in range(32):
            dd = np.arange(-5, 6)
            patch = power[rr[:, None], ((k + dd) % 32)[None, :]]
            training = (np.abs(rr[:, None] - r) > 2) | (np.abs(dd)[None, :] > 1)
            noise = max(float(patch[training].mean()), 1e-16)
            signal = float(power[r, k])
            if signal <= threshold * noise:
                continue
            is_peak = True
            for other_r in range(max(6, r - 1), min(205, r + 2)):
                for delta in (-1, 0, 1):
                    other_k = (k + delta) % 32
                    other = float(power[other_r, other_k])
                    if other > signal or (other == signal and (other_r, other_k) < (r, k)):
                        is_peak = False
                        break
                if not is_peak:
                    break
            if is_peak:
                found.append((r, k, signal, noise))
    found.sort(key=lambda x: (-x[2], x[0], x[1]))
    return found[:32], len(found)


def steering_grid() -> np.ndarray:
    az = np.deg2rad(AZIMUTHS)[:, None]
    el = np.deg2rad(ELEVATIONS)[None, :]
    ux = np.sin(az) * np.cos(el)
    uz = np.broadcast_to(np.sin(el), ux.shape)
    phase = 2.0 * np.pi / WAVELENGTH * (ux[..., None] * POSITIONS[:, 0] + uz[..., None] * POSITIONS[:, 1])
    return np.exp(1j * phase)


def estimate_point(z: np.ndarray, r: int, k: int, signal: float, noise: float,
                   calibration: Calibration, steering: np.ndarray | None = None) -> dict:
    z = np.asarray(z)
    if z.shape != (12,) or not np.all(np.isfinite(z)) or not 6 <= r <= 204 or not 0 <= k < 32:
        raise ValueError("point requires 12 finite channels and a valid range/Doppler index")
    if not np.isfinite(signal) or signal < 0 or not np.isfinite(noise) or noise < 0:
        raise ValueError("point power/noise must be finite and nonnegative")
    if steering is None:
        steering = steering_grid()
    if steering.shape != (121, 31, 12) or not np.all(np.isfinite(steering)):
        raise ValueError("point steering requires finite [121,31,12]")
    beams = np.abs(np.einsum("aec,c->ae", steering.conj(), z, optimize=False)) ** 2
    a, e = np.unravel_index(int(np.argmax(beams)), beams.shape)
    azimuth, elevation = float(AZIMUTHS[a]), float(ELEVATIONS[e])
    energy = float(np.vdot(z, z).real)
    coherence = float(beams[a, e] / (12.0 * energy)) if energy > 0 else 0.0
    coherence = min(1.0, max(0.0, coherence))
    flags = 0
    signed_k = int(SIGNED_DOPPLER[k])
    if abs(signed_k) >= 15:
        flags |= DOPPLER_EDGE
    if coherence < 0.5:
        flags |= LOW_COHERENCE
    if a in (0, 120) or e in (0, 30):
        flags |= ANGLE_EDGE
    if not flags & (DOPPLER_EDGE | LOW_COHERENCE):
        if a not in (0, 120):
            flags |= AZ_VALID
        if e not in (0, 30):
            flags |= EL_VALID
    az_rad, el_rad = np.deg2rad([azimuth, elevation])
    nominal_range = r * BIN_M - calibration.range_bias_m
    horizontal = float(np.rad2deg(np.arcsin(np.sin(az_rad) * np.cos(el_rad))))
    both_angles = (flags & (AZ_VALID | EL_VALID)) == (AZ_VALID | EL_VALID)
    if both_angles and nominal_range > 0:
        flags |= XYZ_VALID
    xyz = bool(flags & XYZ_VALID)
    return {
        "range_bin": r, "doppler_bin": signed_k, "doppler_unsigned": k,
        "range_m": nominal_range, "velocity_mps": signed_k * VELOCITY_BIN,
        "azimuth_deg": azimuth if flags & AZ_VALID else None,
        "elevation_deg": elevation if flags & EL_VALID else None,
        "horizontal_angle_deg": horizontal if both_angles else None,
        "x_m": float(nominal_range * np.cos(el_rad) * np.sin(az_rad)) if xyz else None,
        "y_m": float(nominal_range * np.cos(el_rad) * np.cos(az_rad)) if xyz else None,
        "z_m": float(nominal_range * np.sin(el_rad)) if xyz else None,
        "coherence": coherence, "flags": flags,
        "snr_db": float(10.0 * np.log10(max(signal, 1e-16) / max(noise, 1e-16))),
        "power_dbfs": float(10.0 * np.log10(max(signal, 1e-16))),
    }


def range_angle_heatmap(corrected: np.ndarray) -> np.ndarray:
    """Doppler-integrated 8-element projected-horizontal beam power."""
    corrected = np.asarray(corrected)
    if corrected.shape != (256, 32, 12) or not np.all(np.isfinite(corrected)):
        raise ValueError("RA input requires finite [256,32,12] corrected Doppler cube")
    ux = np.sin(np.deg2rad(AZIMUTHS))
    weights = np.exp(-2j * np.pi / WAVELENGTH * ux[:, None] * POSITIONS[HORIZONTAL, 0])
    beam = np.einsum("ac,rkc->rka", weights, corrected[:205, :, HORIZONTAL], optimize=True)
    normalization = 64.0 * 32.0 * float(np.sum(np.hanning(32) ** 2))
    power = np.sum(np.abs(beam) ** 2, axis=1) / normalization
    dbfs = 10.0 * np.log10(np.maximum(power, 1e-16))
    dbfs[:6] = np.nan
    return dbfs


def analyze(raw: bytes, cfg: bytes, calibration_json: dict | None = None) -> tuple[dict, dict[str, np.ndarray]]:
    calibration = parse_calibration(calibration_json)
    samples, metadata = _codes(raw, cfg)
    cube, doppler, overview = range_and_doppler(samples)
    power = np.mean(np.abs(doppler) ** 2, axis=2) / float(np.hanning(32).sum() ** 2)
    corrected = corrected_channels(doppler, calibration)
    candidates, total = cfar_candidates(power)
    steering = steering_grid()
    points = [estimate_point(corrected[r, k], r, k, signal, noise, calibration, steering)
              for r, k, signal, noise in candidates]
    heatmap = range_angle_heatmap(corrected)
    report = {
        "schema": 1, "status": "OFFLINE_SPATIAL_NUMPY_REFERENCE_PASS",
        "array_id": ARRAY_ID, "cfg_sha256": CFG_SHA256,
        "raw_sha256": hashlib.sha256(raw).hexdigest(), "raw_bytes": len(raw),
        "raw_crc32": metadata["input_crc32"], "hsi_records": len(metadata["hsi_records"]),
        "calibrated": calibration.measured, "calibration_id": calibration.calibration_id,
        "candidate_count": total, "count": len(points), "truncated_count": max(total - 32, 0),
        "xyz_count": sum(bool(p["flags"] & XYZ_VALID) for p in points), "points": points,
        "center_frequency_hz": FC, "wavelength_m": WAVELENGTH,
        "range_bin_m": BIN_M, "velocity_bin_mps": VELOCITY_BIN,
        "tx_sequence": list(TX_SEQUENCE), "channel_axis": "physical TX-major [TX0,TX1,TX2] each RX0..3",
        "cfar": {"range_train_each_side": 8, "range_guard_each_side": 2,
                 "doppler_train_each_side": 4, "doppler_guard_each_side": 1,
                 "threshold_db": 12, "nms": [3, 3], "range_bins": [6, 204],
                 "doppler_wrap": True, "boundary_training": "valid range bins only"},
        "angle_grid_deg": {"azimuth": [-60, 60, 1], "elevation": [-15, 15, 1]},
        "flags": {"AZ_VALID": 1, "EL_VALID": 2, "XYZ_VALID": 4,
                  "DOPPLER_EDGE": 8, "LOW_COHERENCE": 16, "ANGLE_EDGE": 32},
        "limits": ["Saved input replay, not new acquisition or physical accuracy evidence.",
                   "Numerical XYZ_VALID is distinct from measured channel calibration.",
                   "One angular maximum per retained range-Doppler candidate; no multi-target separation within a cell.",
                   "Sensor-relative coordinates; no mount transform, room map or tracking.",
                   "One degree search spacing is not one degree angular resolution.",
                   "RA map uses projected horizontal angle, not elevation-corrected azimuth.",
                   "No synthetic movement is added to captured data."]}
    return report, {"range_cube": cube, "doppler_cube": doppler, "corrected_cube": corrected,
                    "rd_power": power, "overview_dbfs": overview, "heatmap_dbfs": heatmap}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("--cfg", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True, help="JSON report path")
    parser.add_argument("--calibration", type=Path)
    parser.add_argument("--arrays-dir", type=Path, help="optional uncompressed .npy files for comparison")
    args = parser.parse_args()
    try:
        cal = json.loads(args.calibration.read_text(encoding="utf-8-sig")) if args.calibration else None
        result, arrays = analyze(args.input.read_bytes(), args.cfg.read_bytes(), cal)
        result["input_file"] = str(args.input.resolve())
        if args.arrays_dir:
            args.arrays_dir.mkdir(parents=True, exist_ok=True)
            for name, array in arrays.items():
                np.save(args.arrays_dir / (name + ".npy"), array, allow_pickle=False)
            result["array_files"] = {key: str((args.arrays_dir / (key + ".npy")).resolve()) for key in arrays}
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, indent=2, allow_nan=False) + "\n", encoding="utf-8")
        print(json.dumps({key: result[key] for key in ("status", "candidate_count", "count", "xyz_count", "calibrated")}))
        return 0
    except (OSError, ValueError, TypeError, decode_adc.DecodeError) as error:
        print(f"SPATIAL_ANALYSIS_FAILED {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
