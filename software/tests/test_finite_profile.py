#!/usr/bin/env python3
"""Host-only finite-frame admission tests; synthetic files never enter src/config."""
from __future__ import annotations
import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("finite_generator", ROOT / "tools/generate_capture_profile.py")
G = importlib.util.module_from_spec(spec)
spec.loader.exec_module(G)
FW = b"HOST ONLY - never flash this fixture"
CFG = b"""sensorStop
flushCfg
dfeDataOutputMode 1
channelCfg 15 5 0
adcCfg 2 1
adcbufCfg -1 0 1 1 1
profileCfg 0 60 7 3 24 0 0 166 1 256 12500 0 0 158
chirpCfg 0 0 0 0 0 0 0 1
chirpCfg 1 1 0 0 0 0 0 4
bpmCfg -1 0 0 0
lowPower 0 0
frameCfg 0 1 32 1 100 1 0
lvdsStreamCfg -1 1 1 0
sensorStart
"""
CFG3 = CFG.replace(b"channelCfg 15 5 0", b"channelCfg 15 7 0").replace(
    b"chirpCfg 1 1 0 0 0 0 0 4", b"chirpCfg 1 1 0 0 0 0 0 4\nchirpCfg 2 2 0 0 0 0 0 2").replace(
    b"frameCfg 0 1 32 1 100 1 0", b"frameCfg 0 2 32 1 100 1 0")


class FiniteProfileTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.directory = Path(self.temp.name)
        self.addCleanup(self.temp.cleanup)
        evidence = self.directory / "host-source.txt"
        evidence.write_bytes(b"HOST ONLY source identity test fixture")
        self.review = dict(schema=1, qualification="FINITE_FRAME_TEST", name="HOST_ONLY_FINITE",
            firmware_sha256=G.sha(FW), cfg_sha256=G.sha(CFG), version_substring="03.05.00.04",
            lanes=2, lane_mbps=600, word_bits=16, format_flags=8, packet_bytes=266240,
            packet_count=1, skip_pairs=0, startup_mode="REVIEWED_PREAMBLE",
            receive_timeout_us=4000000, snapshot_timeout_us=500000, use_delay=False, taps=[0, 0, 0],
            approvals={key: {"approved": i in (0, 1, 7), "evidence": "HOST ONLY evidence fixture; no physical claim"}
                       for i, key in enumerate(G.APPROVALS)},
            source_reviews={key: {"approved": True, "summary": "HOST ONLY frozen source fixture",
                                  "evidence": [{"path": evidence.name, "sha256": G.sha(evidence.read_bytes())}]}
                            for key in G.SOURCE_REVIEWS})

    def generate(self, cfg=CFG, **changes):
        r = copy.deepcopy(self.review)
        r.update(cfg_sha256=G.sha(cfg), **changes)
        return G.generate(FW, cfg, json.dumps(r).encode(), self.directory)

    def test_three_supported_geometries_keep_pending_approvals(self):
        identities = set()
        for hsi, frames, size, bd_count in ((1, 1, 266240, 5), (0, 1, 262144, 4), (0, 16, 262144, 64)):
            cfg = CFG.replace(b"lvdsStreamCfg -1 1 1 0", f"lvdsStreamCfg -1 {hsi} 1 0".encode())
            cfg = cfg.replace(b"frameCfg 0 1 32 1 100 1 0", f"frameCfg 0 1 32 {frames} 100 1 0".encode())
            with self.subTest(hsi=hsi, frames=frames):
                header, m = self.generate(cfg, packet_bytes=size, packet_count=frames)
                self.assertEqual((m["qualification"], m["approvals"], m["source_review_flags"]), ("FINITE_FRAME_TEST", 0x83, 7))
                self.assertIn(".qualification=2U", header)
                geo = m["finite_geometry"]
                self.assertEqual((geo["source_frames"], geo["hsi_header_enabled"], geo["wire_bytes_per_frame"]), (frames, hsi, size))
                self.assertEqual(geo["adc_bytes_per_chirp"], 4096)
                self.assertEqual(geo["chirps_per_frame"], 64)
                self.assertEqual(((size + 65535) // 65536) * frames, bd_count)
                identities.add(m["profile_id"])
        self.assertEqual(len(identities), 3)

    def test_three_tx_single_hsi_geometry_and_generated_identity(self):
        header, m = self.generate(CFG3, packet_bytes=399360)
        g = m['finite_geometry']
        self.assertEqual((g['chirps_per_frame'], g['tx_count'], g['tx_mask'], g['loops']), (96, 3, 7, 32))
        self.assertEqual(g['tx_sequence'], [0, 2, 1])
        self.assertEqual(g['wire_bytes_per_frame'], 399360)
        self.assertEqual((399360 + 65535) // 65536, 7)
        generated = G.geometry_header(m)
        self.assertIn(m['profile_id'], generated)
        self.assertIn(G.sha(CFG3), generated)
        self.assertIn('TX_SEQUENCE {0U,2U,1U}', generated)
        self.assertEqual(m['approvals'], 0x83)
        for old, new in ((b'channelCfg 15 7 0', b'channelCfg 15 5 0'),
                         (b'0 0 0 0 2', b'0 0 0 0 1'),
                         (b'frameCfg 0 2 32 1', b'frameCfg 0 2 32 16'),
                         (b'frameCfg 0 2 32 1', b'frameCfg 0 2 32 0'),
                         (b'lvdsStreamCfg -1 1 1 0', b'lvdsStreamCfg -1 0 1 0')):
            with self.subTest(new=new), self.assertRaises(G.ProfileError):
                self.generate(CFG3.replace(old, new), packet_bytes=399360)
        with self.assertRaises(G.ProfileError): self.generate(CFG3, packet_bytes=266240)

    def test_packet_geometry_cannot_hide_source_geometry_change(self):
        for before, after in ((b"15 5 0", b"7 5 0"), (b"adcCfg 2 1", b"adcCfg 2 0"),
            (b"adcbufCfg -1 0 1 1 1", b"adcbufCfg -1 0 0 1 1"),
            (b"adcbufCfg -1 0 1 1 1", b"adcbufCfg -1 0 1 0 1"),
            (b"adcbufCfg -1 0 1 1 1", b"adcbufCfg -1 0 1 1 2"),
            (b"256 12500", b"128 12500"), (b"0 0 0 0 4", b"0 0 0 0 2"),
            (b"32 1 100", b"16 1 100"), (b"32 1 100", b"32 0 100"),
            (b"32 1 100", b"32 2 100"), (b"32 1 100", b"32 1 50"),
            (b"bpmCfg -1 0 0 0", b"bpmCfg -1 1 0 1"),
            (b"lvdsStreamCfg -1 1 1 0", b"lvdsStreamCfg -1 1 1 1"),
            (b"lvdsStreamCfg -1 1 1 0", b"lvdsStreamCfg -1 1 4 0")):
            self.assertIn(before, CFG)
            with self.subTest(after=after), self.assertRaises(G.ProfileError):
                self.generate(CFG.replace(before, after))

    def test_duplicate_missing_and_advanced_geometry_rejected(self):
        commands = [b"channelCfg 15 5 0", b"chirpCfg 0 0 0 0 0 0 0 1", b"bpmCfg -1 0 0 0",
                    b"lowPower 0 0", b"frameCfg 0 1 32 1 100 1 0", b"lvdsStreamCfg -1 1 1 0"]
        for command in commands:
            for cfg in (CFG.replace(command+b"\n", b""), CFG.replace(command+b"\n", command+b"\n"+command+b"\n")):
                with self.subTest(command=command, cfg=cfg), self.assertRaises(G.ProfileError): self.generate(cfg)
        for extra in (b"advFrameCfg 1 0 1 1 0", b"subFrameCfg 0 0 0 1 32 100 0 1 1 100", b"flushCfg invalid"):
            with self.subTest(extra=extra), self.assertRaises(G.ProfileError):
                self.generate(CFG.replace(b"sensorStart", extra+b"\nsensorStart"))

    def test_crossed_review_packet_and_cfg_rejected(self):
        for fields in (dict(packet_bytes=262144), dict(packet_bytes=131072), dict(packet_count=16),
                       dict(format_flags=0), dict(skip_pairs=1), dict(startup_mode="IDLE_CLOCK"),
                       dict(use_delay=True), dict(receive_timeout_us=4000001)):
            with self.subTest(fields=fields), self.assertRaises(G.ProfileError): self.generate(**fields)
        cfg = CFG.replace(b"32 1 100", b"32 16 100")
        with self.assertRaises(G.ProfileError): self.generate(cfg, packet_count=16)

    def test_no_approval_or_evidence_promotion(self):
        for i in (0, 1, 7):
            approvals = copy.deepcopy(self.review["approvals"])
            approvals[G.APPROVALS[i]]["approved"] = False
            with self.subTest(i=i), self.assertRaises(G.ProfileError): self.generate(approvals=approvals)
        for key in G.SOURCE_REVIEWS:
            source = copy.deepcopy(self.review["source_reviews"])
            source[key]["evidence"][0]["sha256"] = "0"*64
            with self.subTest(key=key), self.assertRaises(G.ProfileError): self.generate(source_reviews=source)
        with self.assertRaises(G.ProfileError): self.generate(qualification="QUALIFIED")


if __name__ == "__main__":
    unittest.main(verbosity=2)
