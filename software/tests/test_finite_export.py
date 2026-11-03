#!/usr/bin/env python3
"""Verify actual C exporter output, not a hand-written protocol fixture."""
from pathlib import Path
import importlib.util
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("finite_export_decoder", ROOT / "tools/decode_capture.py")
D = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = D
spec.loader.exec_module(D)


class FiniteExportTests(unittest.TestCase):
    def test_four_c_exports_round_trip(self):
        captures = D.captures_from_log(LOG)
        self.assertEqual(len(captures), 4)
        for k, capture in enumerate(captures):
            expected_size = (266240, 262144, 4194304, 399360)[k]
            self.assertEqual(capture.session, 701+k)
            self.assertEqual(capture.size, expected_size)
            self.assertEqual(capture.metadata["qualification"], "FINITE_FRAME_TEST")
            self.assertEqual(capture.metadata["finite_source"], ("1 1 64 256 15", "1 0 64 256 15", "16 0 64 256 15", "1 1 96 256 15")[k])
            self.assertEqual(capture.metadata["review_bits"], "83 07")
            self.assertEqual(bytes(capture.payload), bytes((i*37+k*53+11)&255 for i in range(expected_size)))

    def test_finite_source_required_and_bound_to_geometry(self):
        for before, after in (("@R4META finite_source 1 1 64 256 15", "@R4META finite_source 1 0 64 256 15"),
                              ("@R4META finite_source 1 1 64 256 15", "@R4META unknown_source 1 1 64 256 15"),
                              ("@R4META finite_source 16 0 64 256 15", "@R4META finite_source 1 0 64 256 15")):
            with self.subTest(after=after), self.assertRaises(D.DecodeError):
                D.captures_from_log(LOG.replace(before, after, 1))

    def test_pending_scope_cannot_be_upgraded_by_export(self):
        for before, after in (("@R4META review_bits 83 07", "@R4META review_bits 81 07"),
                              ("@R4META validation_scope INPUT_TIMING_AND_WIRE_FORMAT_PENDING",
                               "@R4META validation_scope PROFILE_REVIEWED_NOT_LIVE_MEASUREMENT")):
            with self.subTest(after=after), self.assertRaises(D.DecodeError):
                D.captures_from_log(LOG.replace(before, after, 1))

    def test_additional_approvals_do_not_change_trial_scope(self):
        captures = D.captures_from_log(LOG.replace("@R4META review_bits 83 07", "@R4META review_bits ff 07"))
        for capture in captures:
            self.assertEqual(capture.metadata["qualification"], "FINITE_FRAME_TEST")
            self.assertEqual(capture.metadata["validation_scope"], "INPUT_TIMING_AND_WIRE_FORMAT_PENDING")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_finite_export.py ACTUAL_C_EXPORT_LOG")
    LOG = Path(sys.argv.pop()).read_text(encoding="ascii")
    unittest.main(verbosity=2)
