"""Pure generated profile geometry/identity checks, no board evidence."""
import importlib.util
import json
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('profile_generator', ROOT / 'tools/generate_capture_profile.py')
G = importlib.util.module_from_spec(spec)
spec.loader.exec_module(G)

class StreamProfiles(unittest.TestCase):
    def test_distinct_strict_profiles(self):
        p = ROOT / 'radar_runtime/src/config/profiles'
        ids = set()
        for count in (0, 16, 64):
            raw = (p / f'radar_3tx_hsi_stream{count}.cfg').read_bytes()
            review = (p / f'radar_3tx_hsi_stream{count}.review.json').read_bytes()
            r = json.loads(review)
            m = json.loads((p / f'radar_capture_stream{count}.manifest.json').read_text())
            ids.add(m['profile_id'])
            self.assertEqual(m['cfg_sha256'], G.sha(raw))
            self.assertEqual(m['review_sha256'], G.sha(review))
            self.assertEqual(r['qualification'], 'STREAM_FRAME_TEST')
            cmd = G.cfg_commands(raw)
            geometry = G.stream_geometry(cmd, 399360, count)
            self.assertEqual(geometry['source_frames'], count)
            self.assertEqual(geometry['stream_window_frames'], 18)
            with self.assertRaises(G.ProfileError):
                G.stream_geometry(cmd, 399352, count)
            with self.assertRaises(G.ProfileError):
                G.stream_geometry(cmd + [f'frameCfg 0 2 32 {count} 100 1 0'], 399360, count)
            with self.assertRaises(G.ProfileError):
                G.stream_geometry([x.replace('channelCfg 15 7 0', 'channelCfg 15 5 0') for x in cmd],399360,count)
            if count != 16:
                with self.assertRaises(G.ProfileError):
                    G.finite_geometry(cmd, 399360, count)
        self.assertEqual(len(ids), 3)

if __name__ == '__main__':
    unittest.main()
