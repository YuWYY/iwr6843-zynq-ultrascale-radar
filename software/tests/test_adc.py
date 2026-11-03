#!/usr/bin/env python3
"""HOST-ONLY decoder fixtures; these tests do not establish real ADC validity."""
from __future__ import annotations

import copy
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("decode_adc", ROOT / "tools" / "decode_adc.py")
D = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = D
spec.loader.exec_module(D)


def cfg(mode="hsi", frames=1, tx=2):
    result = ("% HOST MODEL ONLY: not a captured radar frame.\n"
            "sensorStop\nflushCfg\ndfeDataOutputMode 1\nchannelCfg 15 5 0\nadcCfg 2 1\n"
            "adcbufCfg -1 0 1 1 1\nlowPower 0 0\n"
            "profileCfg 0 60 7 3 24 0 0 166 1 256 12500 0 0 158\n"
            "chirpCfg 0 0 0 0 0 0 0 1\nchirpCfg 1 1 0 0 0 0 0 4\n"
            f"frameCfg 0 1 32 {frames} 100 1 0\n"
            f"lvdsStreamCfg -1 {int(mode == 'hsi')} 1 0\n"
            "bpmCfg -1 0 0 0\nsensorStart\n").encode()
    if tx == 3:
        result = result.replace(b'channelCfg 15 5 0', b'channelCfg 15 7 0').replace(
            b'chirpCfg 1 1 0 0 0 0 0 4', b'chirpCfg 1 1 0 0 0 0 0 4\nchirpCfg 2 2 0 0 0 0 0 2').replace(
            b'frameCfg 0 1 32', b'frameCfg 0 2 32')
    return result


def header(ident=0x0CDA0ADC0CDA0ADC):
    # Independent official-struct fixture: 16B data-card, 36B SDK, 12B padding.
    return bytearray(struct.pack("<QHHIHH6B9H8s12s", ident, 0x30, 1, 0,
                                 0x744, 32, 4, 2, 0, 2, 15, 1,
                                 1, 512, 0, 0, 0, 0, 0, 0, 0,
                                 b"\0" * 8, b"\x0f" * 12))


def serialize_two_lanes(memory):
    # Explicit two-lane serializer, independent of decoder slice operations.
    out = bytearray()
    for base in range(0, len(memory), 16):
        group = struct.unpack_from("<8H", memory, base)
        lane0 = [group[i] for i in (0, 1, 4, 5)]
        lane1 = [group[i] for i in (2, 3, 6, 7)]
        for a, b in zip(lane0, lane1):
            out.extend(struct.pack("<HH", a, b))
    return bytes(out)


def fixture(mode="hsi", ident=0x0CDA0ADC0CDA0ADC, mutate=None, chirps=64):
    memory = bytearray()
    expected = bytearray()
    for chirp in range(chirps):
        if mode == "hsi":
            h = header(ident)
            if mutate:
                mutate(chirp, h)
            memory.extend(h)
        for rx in range(4):
            for sample in range(256):
                # Asymmetric values include negative, positive, sign boundaries
                # and distinct RX/chirp/sample coordinates; I and Q never alias.
                i = ((chirp * 977 + rx * 4099 + sample * 257) & 65535) - 32768
                q = ((chirp * 421 + rx * 7919 + sample * 113 + 32767) & 65535) - 32768
                memory.extend(struct.pack("<hh", q, i))
                expected.extend(struct.pack("<hh", i, q))
    return serialize_two_lanes(memory), bytes(expected)


class AdcTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.hsi, cls.iq = fixture()
        cls.adc, cls.adc_iq = fixture("adc")

    def test_signed_asymmetric_fixed_iq_map(self):
        raw = struct.pack("<4h", -32768, 32767, -12345, 23456)
        actual = D.memory_qi_to_iq(D.raw16_to_memory(raw))
        self.assertEqual(struct.unpack("<4h", actual), (-12345, -32768, 23456, 32767))

    def test_three_tx_records_iq_and_physical_tx_mapping(self):
        raw, iq = fixture(chirps=96)
        g = D.parse_cfg(cfg(tx=3), 'hsi')
        decoded, report = D.decode(raw, g)
        self.assertEqual((len(raw), len(iq), decoded), (399360, 393216, iq))
        self.assertEqual(report['shape'], [1, 96, 4, 256, 2])
        self.assertEqual(report['tdm_view_shape'], [1, 32, 3, 4, 256, 2])
        self.assertEqual(report['tx_for_chirp'], [0, 2, 1]*32)
        self.assertEqual(len(report['hsi_records']), 96)
        broken = bytearray(raw); broken[95*4160] ^= 1
        with self.assertRaisesRegex(D.DecodeError, 'chirp 95'):
            D.decode(broken, g)
        with self.assertRaises(D.DecodeError): D.decode(self.hsi, g)
        with self.assertRaises(D.DecodeError): D.decode(raw, D.parse_cfg(cfg(), 'hsi'))
        with self.assertRaises(D.DecodeError): D.parse_cfg(cfg('adc', tx=3), 'adc')
        with self.assertRaises(D.DecodeError): D.parse_cfg(cfg(tx=3).replace(b'0 0 0 0 2', b'0 0 0 0 4'), 'hsi')

    def test_hsi_full_finite_frame_roundtrip(self):
        actual, report = D.decode(self.hsi, D.parse_cfg(cfg(), "hsi"))
        self.assertEqual(actual, self.iq)
        self.assertEqual(len(actual), 262144)
        self.assertEqual(report["input_bytes"], 266240)
        self.assertEqual(report["shape"], [1, 64, 4, 256, 2])
        self.assertEqual(len(report["hsi_records"]), 64)
        self.assertEqual(report["hsi_records"][-1]["offset"], 63 * 4160)
        self.assertEqual(report["tx_for_chirp"], [0, 2] * 32)
        self.assertFalse(report["hsi_id_is_frame_counter"])
        self.assertIsNone(report["transport_valid"])
        self.assertIn("REQUIRES_FINITE_CAPTURE", report["adc_alignment"])

    def test_adc_only_does_not_claim_alignment(self):
        actual, report = D.decode(self.adc, D.parse_cfg(cfg("adc"), "adc"))
        self.assertEqual(actual, self.adc_iq)
        self.assertEqual(report["adc_alignment"], "NOT_PROVEN_BY_ADC_ONLY_LENGTH")
        self.assertEqual(report["hsi_structure"], "NOT_PRESENT")
        self.assertEqual(report["adc_signal_valid"], "NOT_ESTABLISHED_BY_PARSER")

    def test_adc_sixteen_frames_exact_size(self):
        actual, report = D.decode(self.adc * 16, D.parse_cfg(cfg("adc", 16), "adc"))
        self.assertEqual(actual, self.adc_iq * 16)
        self.assertEqual(report["shape"][0], 16)

    def test_all_legal_hsi_session_ids(self):
        for ident in D.HSI_IDS:
            with self.subTest(ident=hex(ident)):
                info = D.parse_header(header(ident), 0)
                self.assertEqual(info["session_id"], f"0x{ident:016x}")

    def test_every_header_field_is_checked(self):
        changes = [
            (0, "<Q", 0), (8, "<H", 49), (8, "<H", 0x1030), (10, "<H", 0x1001),
            (12, "<I", 1), (16, "<H", 0x745), (18, "<H", 26),
            (20, "<B", 3), (21, "<B", 1), (22, "<B", 1), (23, "<B", 1),
            (24, "<B", 7), (25, "<B", 2), (26, "<H", 2), (28, "<H", 256),
            (30, "<H", 1), (32, "<H", 1), (34, "<H", 1), (36, "<H", 1),
            (38, "<H", 1), (40, "<H", 1), (42, "<H", 1),
            (44, "<B", 1), (51, "<B", 1), (52, "<B", 0), (63, "<B", 0),
        ]
        for offset, fmt, value in changes:
            with self.subTest(offset=offset, value=value):
                h = header()
                struct.pack_into(fmt, h, offset, value)
                with self.assertRaises(D.DecodeError):
                    D.parse_header(h, 7)

    def test_header_is_unmapped_before_parse_not_iq_swapped(self):
        h = header()
        physical = serialize_two_lanes(h)
        self.assertNotEqual(physical, h)
        with self.assertRaises(D.DecodeError):
            D.parse_header(physical, 0)
        self.assertEqual(D.raw16_to_memory(physical), h)
        with self.assertRaises(D.DecodeError):
            D.parse_header(D.memory_qi_to_iq(h), 0)

    def test_late_bad_header_is_not_skipped(self):
        broken = bytearray(self.hsi)
        broken[63 * 4160] ^= 1
        with self.assertRaisesRegex(D.DecodeError, "chirp 63 offset 262080"):
            D.decode(broken, D.parse_cfg(cfg(), "hsi"))

    def test_changed_session_id_is_rejected(self):
        broken = bytearray(self.hsi)
        broken[4160:4224] = serialize_two_lanes(header(0x09CC0CC909CC0CC9))
        with self.assertRaisesRegex(D.DecodeError, "session ID changed"):
            D.decode(broken, D.parse_cfg(cfg(), "hsi"))

    def test_no_offset_search_or_trailing_bytes(self):
        geometry = D.parse_cfg(cfg(), "hsi")
        for broken in (self.hsi[8:] + b"\0" * 8, self.hsi[:-1], self.hsi + b"\0" * 8):
            with self.assertRaises(D.DecodeError):
                D.decode(broken, geometry)
        with self.assertRaises(D.DecodeError):
            D.raw16_to_memory(b"short")
        with self.assertRaises(D.DecodeError):
            D.parse_header(b"short", 0)

    def test_cfg_rejects_unreviewed_geometry(self):
        changes = [(b"channelCfg 15 5 0", b"channelCfg 7 5 0"),
                   (b"adcCfg 2 1", b"adcCfg 1 1"),
                   (b"adcbufCfg -1 0 1 1 1", b"adcbufCfg -1 0 0 1 1"),
                   (b"frameCfg 0 1 32 1", b"frameCfg 0 1 32 0"),
                   (b"frameCfg 0 1 32 1", b"frameCfg 0 1 32 16"),
                   (b"frameCfg 0 1 32 1", b"frameCfg 0 1 16 1"),
                   (b" 256 12500", b" 128 12500"),
                   (b"chirpCfg 1 1 0 0 0 0 0 4", b"chirpCfg 1 1 0 0 0 0 0 1"),
                   (b"lvdsStreamCfg -1 1 1 0", b"lvdsStreamCfg -1 1 1 1"),
                   (b"bpmCfg -1 0", b"bpmCfg -1 1"),
                   (b" 100 1 0", b" 99 1 0"), (b"sensorStart", b"sensorStart 0")]
        for old, new in changes:
            with self.subTest(new=new):
                with self.assertRaises(D.DecodeError):
                    D.parse_cfg(cfg().replace(old, new), "hsi")
        for extra in (b"adcCfg 2 1\n", b"subFrameCfg 0\n", b"advFrameCfg 0\n"):
            with self.assertRaises(D.DecodeError):
                D.parse_cfg(cfg() + extra, "hsi")

    def test_transport_identity_and_evidence_scope(self):
        _, report = D.decode(self.hsi, D.parse_cfg(cfg(), "hsi"))
        exported = {"validation": "SERIAL_EXPORT_LENGTH_CRC_ONLY", "bytes": len(self.hsi),
                    "crc32": f"{zlib.crc32(self.hsi):08x}", "session": 5,
                    "qualification": "FINITE_FRAME_TEST", "metadata": {
                        "cfg_sha256_expected": D.sha256(cfg()), "geometry": "266240 1 8 0 2"}}
        D.bind_transport(report, exported)
        self.assertTrue(report["transport_valid"])
        self.assertIn("REQUIRES_FINITE_CAPTURE", report["adc_alignment"])
        for field, value in [("bytes", 0), ("crc32", "00000000"), ("validation", "PASS")]:
            bad = copy.deepcopy(exported)
            bad[field] = value
            with self.assertRaises(D.DecodeError):
                D.bind_transport(report, bad)
        for field, value in [("cfg_sha256_expected", "0" * 64),
                             ("geometry", "266240 1 8 4 2"),
                             ("geometry", "266240 1 0 0 2")]:
            bad = copy.deepcopy(exported)
            bad["metadata"][field] = value
            with self.assertRaises(D.DecodeError):
                D.bind_transport(report, bad)

    def test_cli_writes_only_validated_output_and_protects_existing(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            inp, cfgpath, out = base / "host.bin", base / "host.cfg", base / "decoded"
            inp.write_bytes(self.hsi)
            cfgpath.write_bytes(cfg())
            command = [sys.executable, str(ROOT / "tools" / "decode_adc.py"), str(inp),
                       "--mode", "hsi", "--cfg", str(cfgpath), "--output", str(out)]
            run = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)
            binary = base / "decoded.iq_i16le.bin"
            self.assertEqual(binary.read_bytes(), self.iq)
            report = json.loads((base / "decoded.adc.json").read_text())
            self.assertEqual(report["iq_sha256"], D.sha256(self.iq))
            repeat = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(repeat.returncode, 2)
            self.assertIn("output exists", repeat.stderr)
            inp.write_bytes(self.hsi[:-8])
            command[-1] = str(base / "bad")
            failed = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(failed.returncode, 2)
            self.assertFalse((base / "bad.adc.json").exists())
            self.assertFalse((base / "bad.iq_i16le.bin").exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
