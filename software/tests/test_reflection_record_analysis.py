"""Synthetic schema fixture and metric tests, never a live radar data source."""
import copy
import importlib.util
import json
import math
from pathlib import Path
import shutil
import struct
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('reflection_analysis', ROOT / 'tools/analyze_reflection_record.py')
analysis = importlib.util.module_from_spec(spec)
spec.loader.exec_module(analysis)


def image_fixture(bearing=False, padded_points=False):
    """Independent closed-form stationary-point expectations, with missing
    observations, real expiry and a recorded outer MAP_CHANGE reset."""
    n = 601
    frames, timing, heats = [], [], []
    seed, current_id, first_seen, last_seen = 47, 0, 0, 0
    next_id = seed
    for i in range(n):
        now = 1000000 + 100000 * i
        present = not 351 <= i <= 357
        point_count = (31 if padded_points else 0) + int(present)
        frame = bytearray(2560)
        frame[:4] = b'RSF1'
        struct.pack_into('<I6I2Q16IQI', frame, 4, 2560, i + 1, 7, i + 100, i + 1, i + 0x12340000, 7,
                         now, now + 200, 35, 3, 1, 0, point_count, point_count, 0, 0,
                         4 if i == 400 else 0, 4 if i == 400 else 0, 0, 0, 0, 0, 1, 1, i + 1, 0)
        if present:
            qflags = 64 | 16 | 32 if bearing else 0
            struct.pack_into('<Ii11fI', frame, 128 + (31 * 56 if padded_points else 0), 61, 0, 2.68, 0, math.nan, math.nan,
                             15 if bearing else math.nan, math.nan, math.nan, math.nan, 20, .2, -45, qflags)
        if current_id and now - last_seen > 500000:
            current_id = 0
        if present:
            if not current_id:
                current_id, next_id, first_seen = next_id, next_id + 1, now
            last_seen = now
        if current_id:
            state = (2 if now - first_seen >= 2000000 else 1) if present else 3
            flags = (1 if present else 0) | (2 if bearing else 0)
            u = math.sin(math.radians(15)) if bearing else math.nan
            struct.pack_into('<I', frame, 92, 1)
            struct.pack_into('<IIffIf', frame, 2112, current_id, state | flags << 8, 2.68, u, (now - last_seen) // 1000, 0)
        if i == 400:
            current_id = 0
            struct.pack_into('<I', frame, 92, 0)
            frame[2112:2136] = bytes(24)
        struct.pack_into('<I', frame, 2432, 1)
        struct.pack_into('<I', frame, 2512, 123)
        struct.pack_into('<I', frame, 124, zlib.crc32(frame[:124] + frame[128:]))
        frames.append(frame)
        t = bytearray(320)
        struct.pack_into('<Q', t, 0, i + 1)
        for offset, value in ((8, i + 100), (12, 7), (24, i + 0x12340000), (28, 7), (32, 96), (44, i + 1), (64, i + 1),
                              (224, i + 1), (228, i + 100), (232, i + 1)):
            struct.pack_into('<I', t, offset, value)
        struct.pack_into('<Q', t, 272, now + 50000)
        struct.pack_into('<Q', t, 280, now)
        timing.append(t)
    for index, seconds in enumerate((10, 30, 55)):
        i = seconds * 10
        heat = bytearray(100352)
        heat[:4] = b'RSH1'
        struct.pack_into('<IIIQIIIII', heat, 4, index + 1, i + 1, i + 100, 1000000 + i * 100000,
                         205, 121, seconds, i + 0x12340000, 7)
        struct.pack_into('<I', heat, 44, zlib.crc32(heat[256:99476]))
        struct.pack_into('<I', heat, 252, zlib.crc32(heat[:252]))
        heats.append(heat)
    header = bytearray(4096)
    header[:8] = b'RSCENE01'
    struct.pack_into('<14I2Q', header, 8, 1, 4096, 2560, 320, 100352, 0x01020304, 205, 121,
                     60, 3, 7, n, n, 3, 1000000, 61000000)
    payload = b''.join(frames + timing + heats)
    for offset, value in ((80, 4096 + len(payload)), (92, n), (104, 1), (108, 100), (112, 700),
                          (120, zlib.crc32(payload)), (128, seed)):
        struct.pack_into('<I', header, offset, value)
    for offset, value in ((256, 'TEST_SYNTHETIC_ONLY'), (320, 'TEST_REFLECTION_REPLAY'), (384, 'TEST_PROFILE'),
                          (416, 'TEST_CFG_NOT_FIRMWARE'), (496, 'IWR6843ISK_2P5MM_TX021_V1'),
                          (544, 'UNCALIBRATED'), (608, 'TEST_BIT'), (688, 'TEST_XSA'), (768, 'TEST_FW'),
                          (848, 'TEST_NOT_OBSERVED'), (1104, 'TEST_CLOSED_FORM_EXPECTATIONS')):
        header[offset:offset + len(value)] = value.encode('ascii')
    struct.pack_into('<I', header, 4092, zlib.crc32(header[:4092]))
    return bytes(header) + payload


def write_decoded_fixture(directory, bearing=False, padded_points=False, wrong_seed=False):
    directory = Path(directory)
    directory.mkdir(parents=True)
    image = image_fixture(bearing, padded_points)
    if wrong_seed:
        image = bytearray(image)
        struct.pack_into('<I', image, 128, 48)
        struct.pack_into('<I', image, 4092, zlib.crc32(image[:4092]))
        image = bytes(image)
    manifest, frames, _, _ = analysis.decoder().decode_image(image)
    manifest.update(launch_binding_verified=True, launch_identity={'elf_sha256': 'TEST_NOT_BOARD'}, evidence_kind='TEST')
    (directory / 'image.bin').write_bytes(image)
    analysis.json_write(directory / 'manifest.json', manifest)
    (directory / 'frames.jsonl').write_text(''.join(json.dumps(analysis.clean(frame), allow_nan=False) + '\n' for frame in frames), encoding='utf-8')
    return manifest, analysis.clean(frames)


class ReflectionRecordTests(unittest.TestCase):
    def test_independent_real_schema_and_nulls(self):
        with tempfile.TemporaryDirectory() as tmp:
            manifest, frames = write_decoded_fixture(Path(tmp) / 'decoded')
            loaded, decoded = analysis.load_verified(Path(tmp) / 'decoded')
            self.assertEqual(loaded['reflection_id_seed'], 47)
            self.assertEqual(decoded, frames)
            self.assertIsNone(frames[0]['reflections'][0]['u'])
            self.assertEqual(frames[400]['reflection_count'], 0)
            self.assertEqual(frames[401]['reflections'][0]['id'], 49)
            self.assertTrue(manifest['complete_duration_record'])

    def test_metrics_fixed_window_and_missing_measurements(self):
        _, frames, _, _ = analysis.decoder().decode_image(image_fixture())
        frames = analysis.clean(frames)
        replay = []
        first = {}
        for f in frames:
            refs = []
            for r in f['reflections']:
                first.setdefault(r['id'], f['timestamp_us'])
                refs.append(dict(r, first_observed_us=first[r['id']], last_observed_us=f['timestamp_us'] - r['age_ms'] * 1000,
                                 support_buckets=8, sample_count=21, range_median_m=r['range_m']))
            replay.append(dict(frame_id=f['frame_id'], timestamp_us=f['timestamp_us'], record_index=f['record_index'], reflections=refs))
        result = analysis.metrics(frames, replay)
        self.assertEqual(result['stable_30s']['status'], 'PASS')
        self.assertEqual(result['stable_30s']['primary_id'], 47)
        self.assertEqual(result['stable_30s']['stable_time_fraction'], 1)
        self.assertEqual(result['scene_cpu']['p95_us'], 123)
        self.assertEqual(result['id_changes']['distinct_measured_ids'], [47, 48, 49])
        self.assertEqual(result['missing_measurement_expiry']['expiry_events_observed'], 1)
        self.assertEqual(analysis.metrics(frames[:100], replay[:100])['missing_measurement_expiry']['status'], 'NOT_PROVEN')
        self.assertEqual(analysis.metrics(frames[:100], replay[:100])['stable_30s']['status'], 'NOT_PROVEN')
        bad = copy.deepcopy(frames)
        for f in bad:
            f['diagnostics']['processing_us'] = 0
        self.assertEqual(analysis.metrics(bad, replay)['scene_cpu']['status'], 'NOT_PROVEN')
        for f in bad:
            f['diagnostics']['processing_us'] = 6000
        self.assertEqual(analysis.metrics(bad, replay)['scene_cpu']['status'], 'FAIL')
        for f in bad:
            f['reflections'] = []
        self.assertEqual(analysis.metrics(bad, replay)['stable_30s']['status'], 'NOT_PROVEN')
        renewed = copy.deepcopy(replay)
        renewed[351]['reflections'][0]['last_observed_us'] += 1000
        self.assertEqual(analysis.metrics(frames, renewed)['missing_measurement_expiry']['status'], 'FAIL')

    def test_no_framewise_id_remapping(self):
        frame = dict(record_index=1, frame_id=1, timestamp_us=1000000,
                     reflections=[dict(id=47, state=1, flags=1, age_ms=0, range_m=2.68, u=None, range_mad_m=0)])
        replay = copy.deepcopy(frame)
        self.assertEqual(analysis.compare_replay([frame], [replay])['status'], 'PASS')
        replay['reflections'][0]['id'] += 1
        self.assertEqual(analysis.compare_replay([frame], [replay])['status'], 'FAIL')

    def test_saved_json_and_image_identity_guard(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp) / 'decoded'
            _, frames = write_decoded_fixture(folder)
            frames[0]['reflections'][0]['range_m'] += 1
            (folder / 'frames.jsonl').write_text(''.join(json.dumps(row) + '\n' for row in frames))
            with self.assertRaisesRegex(ValueError, 'frames.jsonl'):
                analysis.load_verified(folder)
            data = bytearray((folder / 'image.bin').read_bytes())
            data[6000] ^= 1
            (folder / 'image.bin').write_bytes(data)
            with self.assertRaisesRegex(ValueError, 'SHA-256'):
                analysis.load_verified(folder)

    def test_missing_seed_is_rejected_not_inferred(self):
        image = bytearray(image_fixture())
        struct.pack_into('<I', image, 128, 0)
        struct.pack_into('<I', image, 4092, zlib.crc32(image[:4092]))
        with self.assertRaisesRegex(ValueError, 'seed'):
            analysis.decoder().decode_image(image)

    def test_replay_source_must_match_recorded_launch_seal(self):
        with tempfile.TemporaryDirectory() as tmp:
            seal = Path(tmp) / 'seal.json'
            binding = {'build_id': 'TEST_SEAL', 'inputs': [{'name': 'app', 'sha256': 'a' * 64}],
                       'source_files': [{'path': str(ROOT / name), 'sha256': analysis.sha(ROOT / name)}
                                        for name in analysis.SOURCE_FILES]}
            analysis.json_write(seal, binding)
            manifest = {'build_id': 'TEST_SEAL', 'launch_identity_sha256': analysis.sha(seal),
                        'launch_identity': {'elf_sha256': 'a' * 64}}
            self.assertEqual(analysis.source_provenance(seal, manifest, 'real')['status'], 'MATCHED_RECORDED_ELF_SOURCE')
            manifest['launch_identity']['elf_sha256'] = 'b' * 64
            with self.assertRaisesRegex(ValueError, 'different application ELF'):
                analysis.source_provenance(seal, manifest, 'real')
            manifest['launch_identity']['elf_sha256'] = 'a' * 64
            binding['source_files'][0]['sha256'] = 'c' * 64
            analysis.json_write(seal, binding)
            with self.assertRaisesRegex(ValueError, 'decoder-bound'):
                analysis.source_provenance(seal, manifest, 'real')
            manifest['launch_identity_sha256'] = analysis.sha(seal)
            with self.assertRaisesRegex(ValueError, 'replay source differs'):
                analysis.source_provenance(seal, manifest, 'real')

    def test_archived_source_root_keeps_seal_guard(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            archive = root / 'archived_sources'
            for name in analysis.SOURCE_FILES:
                target = archive / name
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(ROOT / name, target)
            seal = root / 'original_seal.json'
            binding = {'build_id': 'TEST_ARCHIVE', 'inputs': [{'name': 'app', 'sha256': 'a' * 64}],
                       'source_files': [{'path': str(ROOT / name), 'sha256': analysis.sha(ROOT / name)}
                                        for name in analysis.SOURCE_FILES]}
            analysis.json_write(seal, binding)
            manifest = {'build_id': 'TEST_ARCHIVE', 'launch_identity_sha256': analysis.sha(seal),
                        'launch_identity': {'elf_sha256': 'a' * 64}}
            result = analysis.source_provenance(seal, manifest, 'real', archive)
            self.assertEqual(result['source_root'], str(archive.resolve()))
            self.assertEqual(result['sha256'], {name: analysis.sha(ROOT / name) for name in analysis.SOURCE_FILES})
            selected_header = archive / 'radar_runtime/src/processing/radar_reflection.h'
            selected_header.write_text(selected_header.read_text() + '\n#error ARCHIVE_HEADER_SELECTED\n')
            with self.assertRaisesRegex(ValueError, 'replay source differs'):
                analysis.source_provenance(seal, manifest, 'real', archive)
            out = root / 'compile_guard'
            out.mkdir()
            # Direct compiler check: selected archive headers, not mainline
            # headers adjacent to the current test harness, must be used.
            with self.assertRaisesRegex(ValueError, 'host build failed'):
                analysis.run_replay(root / 'unused_input', out, source_root=archive)
            self.assertIn(b'ARCHIVE_HEADER_SELECTED', (out / 'compile.log').read_bytes())


if __name__ == '__main__':
    unittest.main()
