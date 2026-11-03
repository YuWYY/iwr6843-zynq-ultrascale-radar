#!/usr/bin/env python3
"""Independent synthetic and recorded-data checks; no board or key access."""
from __future__ import annotations

import json
from pathlib import Path
import struct
import sys
import unittest

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import analyze_range
import analyze_spatial as spatial

CFG = ROOT / "verification/20261030_pages_3tx_01/inputs/radar_3tx_hsi_single.cfg"
RAW = ROOT / "verification/20261030_pages_3tx_01/board_sessions/pages_20261030_173429/first_capture.bin"


def synthetic_samples(targets: list[dict], *, seed: int = 476, noise: float = 2.0,
                      gain: np.ndarray | None = None) -> np.ndarray:
    """Create physical-TX IQ; geometry, time offsets and slow FFT are explicit.

    Targets carry range_bin, doppler_bin, azimuth_deg, elevation_deg, amplitude.
    This fixture simulates a point reflector, not a person or hardware capture.
    """
    rng = np.random.default_rng(seed)
    samples = noise * (rng.normal(size=(32, 12, 256)) + 1j * rng.normal(size=(32, 12, 256)))
    # Physical TX0,TX1,TX2 start at slots0,2,1 respectively.
    physical_slot = np.repeat([0.0, 2.0, 1.0], 4)
    time_in_cycles = np.arange(32)[:, None, None] + physical_slot[None, :, None] / 3.0
    for target in targets:
        az, el = np.deg2rad([target["azimuth_deg"], target["elevation_deg"]])
        direction = np.array([np.sin(az) * np.cos(el), np.sin(el)])
        spatial_phase = 2 * np.pi * (spatial.POSITIONS @ direction) / spatial.WAVELENGTH
        fast = 2 * np.pi * target["range_bin"] * np.arange(256) / 256
        slow = 2 * np.pi * target["doppler_bin"] * time_in_cycles / 32
        samples += target["amplitude"] * np.exp(1j * (fast[None, None, :] + slow + spatial_phase[None, :, None]))
    if gain is not None:
        if np.asarray(gain).shape != (12,):
            raise ValueError("synthetic gain must have 12 physical channels")
        samples *= np.asarray(gain)[None, :, None]
    return samples


def synthetic_raw(samples: np.ndarray) -> bytes:
    """Build complete HSI + Q,I memory and independently apply lane permutation."""
    samples = np.asarray(samples)
    if samples.shape != (32, 12, 256) or not np.all(np.isfinite(samples)):
        raise ValueError("synthetic input requires finite [32,12,256]")
    real, imag = np.rint(samples.real), np.rint(samples.imag)
    if np.any(np.abs(real) > 32767) or np.any(np.abs(imag) > 32767):
        raise ValueError("synthetic input would clip")
    # Flatten into actual time slots0,2,1 followed by RX0..3.
    real = real.reshape(32, 3, 4, 256)[:, [0, 2, 1]].reshape(96, 4, 256)
    imag = imag.reshape(32, 3, 4, 256)[:, [0, 2, 1]].reshape(96, 4, 256)
    memory = bytearray()
    for chirp in range(96):
        header = bytearray(64)
        struct.pack_into("<QHHI", header, 0, 0x0CDA0ADC0CDA0ADC, 4144 & 0xFFF, 4144 >> 12, 0)
        struct.pack_into("<HH6B9H", header, 16, 0x0744, 32, 4, 2, 0, 2, 15, 1,
                         1, 512, 0, 0, 0, 0, 0, 0, 0)
        header[52:] = b"\x0f" * 12
        qi = np.stack((imag[chirp], real[chirp]), axis=-1).astype("<i2")
        memory.extend(header)
        memory.extend(qi.tobytes())
    words = np.frombuffer(memory, dtype="<u2").reshape(-1, 4)
    return words[:, [0, 2, 1, 3]].astype("<u2").tobytes()


def target(range_bin=60, doppler_bin=3, azimuth_deg=20, elevation_deg=7, amplitude=2000):
    return dict(range_bin=range_bin, doppler_bin=doppler_bin, azimuth_deg=azimuth_deg,
                elevation_deg=elevation_deg, amplitude=amplitude)


def calibration(gain=None, *, bias=0.0, measured=True):
    coeff = np.ones(12, dtype=complex) if gain is None else 1.0 / np.asarray(gain)
    return dict(array_id=spatial.ARRAY_ID, cfg_sha256=spatial.CFG_SHA256,
                calibration_id="SYNTHETIC_ONLY" if measured else "UNCALIBRATED", measured=measured,
                range_bias_m=bias, channel=np.stack((coeff.real, coeff.imag), axis=1).tolist())


class SpatialTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cfg = CFG.read_bytes()

    def test_single_point_tdm_and_uncalibrated_preview(self):
        raw = synthetic_raw(synthetic_samples([target()]))
        result, arrays = spatial.analyze(raw, self.cfg)
        point = result["points"][0]
        self.assertEqual((point["range_bin"], point["doppler_bin"]), (60, 3))
        self.assertEqual((point["azimuth_deg"], point["elevation_deg"]), (20.0, 7.0))
        self.assertGreater(point["coherence"], 0.99999)
        self.assertEqual(point["flags"], 7)
        self.assertFalse(result["calibrated"])
        self.assertEqual(result["calibration_id"], "UNCALIBRATED")
        self.assertEqual(arrays["heatmap_dbfs"].shape, (205, 121))
        self.assertTrue(np.isnan(arrays["heatmap_dbfs"][:6]).all())
        self.assertAlmostEqual(point["x_m"] ** 2 + point["y_m"] ** 2 + point["z_m"] ** 2,
                               point["range_m"] ** 2, places=12)

    def test_negative_doppler_left_low_and_physical_calibration(self):
        gain = np.linspace(0.65, 1.3, 12) * np.exp(1j * np.linspace(-1.1, 0.9, 12))
        raw = synthetic_raw(synthetic_samples([target(70, -4, -25, -9)], gain=gain))
        result, _ = spatial.analyze(raw, self.cfg, calibration(gain, bias=2 * spatial.BIN_M))
        point = result["points"][0]
        self.assertEqual((point["azimuth_deg"], point["elevation_deg"], point["doppler_bin"]), (-25, -9, -4))
        self.assertAlmostEqual(point["range_m"], 68 * spatial.BIN_M)
        self.assertLess(point["x_m"], 0)
        self.assertLess(point["z_m"], 0)
        self.assertGreater(point["coherence"], 0.99999)

    def test_static_not_removed_and_old_overview_unchanged(self):
        raw = synthetic_raw(synthetic_samples([target(50, 0, 0, 0)]))
        result, arrays = spatial.analyze(raw, self.cfg)
        self.assertEqual(result["points"][0]["doppler_bin"], 0)
        old, _ = analyze_range.analyze(raw, self.cfg)
        np.testing.assert_allclose(arrays["overview_dbfs"], old, rtol=0, atol=1e-11)

    def test_same_range_separate_doppler_targets(self):
        raw = synthetic_raw(synthetic_samples([target(80, 5, 20, 6, 2000), target(80, -5, -25, -7, 1800)]))
        result, _ = spatial.analyze(raw, self.cfg)
        matching = {(p["range_bin"], p["doppler_bin"]): p for p in result["points"]}
        self.assertIn((80, 5), matching)
        self.assertIn((80, -5), matching)
        self.assertEqual(matching[80, 5]["azimuth_deg"], 20)
        self.assertEqual(matching[80, -5]["azimuth_deg"], -25)

    def test_quality_flags_and_no_fake_coordinate(self):
        steering = spatial.steering_grid()
        unity = spatial.parse_calibration(None)
        z = steering[80, 22]
        doppler_edge = spatial.estimate_point(z, 60, 15, 1, 0.001, unity, steering)
        self.assertEqual(doppler_edge["flags"], spatial.DOPPLER_EDGE)
        self.assertIsNone(doppler_edge["x_m"])
        az_edge = spatial.estimate_point(steering[120, 20], 60, 3, 1, 0.001, unity, steering)
        self.assertEqual(az_edge["flags"], spatial.ANGLE_EDGE | spatial.EL_VALID)
        self.assertIsNone(az_edge["azimuth_deg"])
        el_edge = spatial.estimate_point(steering[80, 30], 60, 3, 1, 0.001, unity, steering)
        self.assertEqual(el_edge["flags"], spatial.ANGLE_EDGE | spatial.AZ_VALID)
        self.assertIsNone(el_edge["elevation_deg"])
        sparse = np.zeros(12, dtype=complex); sparse[0] = 1
        low = spatial.estimate_point(sparse, 60, 3, 1, 0.001, unity, steering)
        self.assertTrue(low["flags"] & spatial.LOW_COHERENCE)
        self.assertFalse(low["flags"] & spatial.XYZ_VALID)

    def test_cfar_nms_wrap_bounds_and_order(self):
        matrix = np.ones((256, 32))
        matrix[6, 0] = matrix[6, 31] = 1000
        matrix[204, 10] = 900
        matrix[0, :] = 1e20  # excluded bins must not raise local training noise.
        candidates, count = spatial.cfar_candidates(matrix)
        self.assertEqual(count, 2)
        self.assertEqual([(p[0], p[1]) for p in candidates], [(6, 0), (204, 10)])

    def test_candidate_limit_and_zero_input(self):
        matrix = np.ones((256, 32))
        for index, r in enumerate(range(6, 205, 5)):
            matrix[r, (index % 4) * 8] = 10000 + index
        candidates, count = spatial.cfar_candidates(matrix)
        self.assertEqual(count, 40)
        self.assertEqual(len(candidates), 32)
        self.assertGreater(candidates[0][2], candidates[-1][2])
        result, arrays = spatial.analyze(synthetic_raw(np.zeros((32, 12, 256), dtype=complex)), self.cfg)
        self.assertEqual(result["count"], 0)
        self.assertEqual(result["xyz_count"], 0)
        np.testing.assert_array_equal(arrays["heatmap_dbfs"][6:], -160.0)

    def test_range_bias_cannot_create_negative_xyz(self):
        steering = spatial.steering_grid()
        cal = spatial.parse_calibration(calibration(bias=0.5))
        point = spatial.estimate_point(steering[70, 20], 6, 0, 1, 0.001, cal, steering)
        self.assertLess(point["range_m"], 0)
        self.assertEqual(point["flags"], spatial.AZ_VALID | spatial.EL_VALID)
        self.assertIsNone(point["x_m"])

    def test_normalization_and_horizontal_map(self):
        samples = synthetic_samples([target(64, 0, 0, 0, 2000)], noise=0)
        _, doppler, _ = spatial.range_and_doppler(samples)
        corrected = spatial.corrected_channels(doppler, spatial.parse_calibration(None))
        map_db = spatial.range_angle_heatmap(corrected)
        expected = 20 * np.log10(2000 / 32768)
        # TDM de-rotation of finite-Hann Doppler sidelobes has a very small
        # integration loss; normalization must not yield an 8-channel gain.
        self.assertLess(abs(float(map_db[64, 60]) - expected), 0.02)

    def test_strict_corruption_and_geometry_rejection(self):
        raw = synthetic_raw(synthetic_samples([target()]))
        for bad in (raw[:-8], raw + b"\0" * 8, bytes([raw[0] ^ 1]) + raw[1:]):
            with self.assertRaises(ValueError):
                spatial.analyze(bad, self.cfg)
        with self.assertRaises(ValueError):
            spatial.analyze(raw, self.cfg + b"\n")
        for bad in (np.zeros((31, 12, 256)), np.full((32, 12, 256), np.nan)):
            with self.assertRaises(ValueError):
                spatial.range_and_doppler(bad)

    def test_calibration_identity_and_nonfinite_rejected(self):
        for key, value in (("array_id", "ODS"), ("cfg_sha256", "0" * 64),
                           ("range_bias_m", float("nan")), ("measured", "yes")):
            bad = calibration(); bad[key] = value
            with self.assertRaises(ValueError):
                spatial.parse_calibration(bad)
        bad = calibration(); bad["channel"][2] = [0, 0]
        with self.assertRaises(ValueError):
            spatial.parse_calibration(bad)

    def test_real_capture_overview_and_json_finiteness(self):
        raw = RAW.read_bytes()
        result, arrays = spatial.analyze(raw, self.cfg)
        original, _ = analyze_range.analyze(raw, self.cfg)
        np.testing.assert_allclose(arrays["overview_dbfs"], original, rtol=0, atol=1e-10)
        self.assertEqual(result["hsi_records"], 96)
        self.assertEqual(result["raw_bytes"], 399360)
        self.assertFalse(result["calibrated"])
        self.assertTrue(np.isfinite(arrays["heatmap_dbfs"][6:]).all())
        json.dumps(result, allow_nan=False)


if __name__ == "__main__":
    unittest.main(verbosity=2)
