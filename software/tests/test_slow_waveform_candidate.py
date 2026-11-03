"""Offline candidate generation only; no production profile or board changes."""
import importlib.util
from pathlib import Path
import unittest
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('slow_candidate',ROOT/'tools/generate_slow_waveform_candidate.py')
mod=importlib.util.module_from_spec(spec);spec.loader.exec_module(mod)

class SlowCandidate(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source=(ROOT/'radar_runtime/src/config/profiles/radar_3tx_hsi_stream16.cfg').read_text()
    def test_only_timing_and_velocity_filter_change(self):
        output,meta=mod.make_candidate(self.source)
        commands=lambda t:[x for x in t.splitlines() if x.strip() and not x.lstrip().startswith('%')]
        before,after=commands(self.source),commands(output)
        self.assertEqual(len(before),len(after));self.assertEqual(sum(a!=b for a,b in zip(before,after)),2)
        self.assertEqual(meta['tdm_slot_offsets_us'],[0.0,124.0,248.0]);self.assertEqual(meta['frame_bytes'],399360)
        self.assertAlmostEqual(meta['coherent_observation_span_us'],11904)
        self.assertAlmostEqual(meta['theoretical_doppler_bin_mps'],.20246,places=4)
        self.assertEqual(meta['status'],'NOT_RUNTIME_READY');self.assertFalse(meta['selected'])
        self.assertFalse(meta['runtime_header_generated']);self.assertFalse(meta['hardware_executed'])
    def test_reject_changed_source_waveform(self):
        with self.assertRaises(ValueError):mod.make_candidate(self.source.replace('60 7 3 24','60 8 3 24'))
    def test_reject_different_mapping(self):
        with self.assertRaises(ValueError):mod.make_candidate(self.source.replace('chirpCfg 1 1 0 0 0 0 0 4','chirpCfg 1 1 0 0 0 0 0 2'))
    def test_reject_geometry_change(self):
        with self.assertRaises(ValueError):mod.make_candidate(self.source.replace('frameCfg 0 2 32','frameCfg 0 2 64'))

if __name__=='__main__':unittest.main(verbosity=2)
