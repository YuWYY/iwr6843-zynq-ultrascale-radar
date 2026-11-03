#!/usr/bin/env python3
"""Replay certified point records and report measured stability, not accuracy.

No device access. The existing decoder validates wire CRCs and launch binding.
This tool revalidates its saved image, runs the production C reflection code,
and compares every saved compact reflection. Sparse heatmaps do NOT permit a
complete structural-map replay; recorded map-reset decisions are inputs here.
"""
import argparse
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import statistics
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
SOURCE_FILES = (
    'radar_runtime/src/processing/radar_scene.c',
    'radar_runtime/src/processing/radar_reflection.c',
    'radar_runtime/src/processing/radar_reflection.h',
    'radar_runtime/src/processing/radar_scene_types.h',
    'radar_runtime/src/processing/radar_spatial_types.h',
    'radar_runtime/src/processing/radar_dsp_types.h',
    'radar_runtime/src/processing/radar_range.h',
    'radar_runtime/src/config/radar_capture_geometry.h',
)


def require(condition, text):
    if not condition:
        raise ValueError(text)


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def clean(value):
    if isinstance(value, float) and not math.isfinite(value):
        return None
    if isinstance(value, dict):
        return {key: clean(item) for key, item in value.items()}
    if isinstance(value, (list, tuple)):
        return [clean(item) for item in value]
    return value


def json_write(path, value):
    Path(path).write_text(json.dumps(clean(value), indent=2, allow_nan=False) + '\n', encoding='utf-8')


def decoder():
    spec = importlib.util.spec_from_file_location('scene_record_decoder', ROOT / 'tools/decode_scene_record.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def load_verified(directory):
    """Reparse the CRC-checked binary, never trust a loose edited JSONL alone."""
    directory = Path(directory)
    manifest = json.loads((directory / 'manifest.json').read_text(encoding='utf-8-sig'))
    require(manifest.get('validated') is True, 'decoder did not validate this record')
    require(manifest.get('launch_binding_verified') is True, 'missing verified launch binding')
    image = (directory / 'image.bin').read_bytes()
    require(hashlib.sha256(image).hexdigest() == manifest.get('image_sha256'), 'image SHA-256 changed after decoding')
    decoded, frames, _, _ = decoder().decode_image(image)
    for name in ('image_crc32', 'image_sha256', 'epoch', 'frame_count', 'cfg_sha256',
                 'calibration_id', 'scene_parameters', 'build_id', 'software_version',
                 'bit_sha256', 'xsa_sha256', 'reflection_id_seed', 'error', 'incomplete'):
        require(name in decoded and manifest.get(name) == decoded[name], 'manifest/image mismatch: ' + name)
    require(isinstance(manifest['reflection_id_seed'], int) and 0 < manifest['reflection_id_seed'] <= 0xffffffff,
            'missing actual recorded initial reflection ID seed')
    saved = [json.loads(line) for line in (directory / 'frames.jsonl').read_text(encoding='utf-8').splitlines() if line.strip()]
    require(clean(saved) == clean(frames), 'frames.jsonl differs from decoder-validated image')
    return manifest, clean(frames)


def source_provenance(seal_path, manifest, kind, source_root=ROOT):
    source_root = Path(source_root).resolve()
    actual = {name: sha(source_root / name) for name in SOURCE_FILES}
    if seal_path is None and kind != 'test':
        seal_path = manifest.get('launch_identity_path')
    if seal_path is None:
        require(kind == 'test', 'real record replay requires --source-seal from the sealed launch')
        return {'status': 'TEST_CURRENT_SOURCE_ONLY', 'source_root': str(source_root), 'sha256': actual}
    seal_path = Path(seal_path)
    require(sha(seal_path) == manifest.get('launch_identity_sha256'), 'source seal differs from decoder-bound launch identity')
    seal = json.loads(seal_path.read_text(encoding='utf-8-sig'))
    require(seal.get('build_id') == manifest['build_id'], 'source seal build differs from recorded build')
    rows = seal.get('source_files', [])
    for name, digest in actual.items():
        matches = [row for row in rows if str(row.get('path', '')).replace('\\', '/').endswith('/' + name)]
        require(len(matches) == 1 and matches[0].get('sha256') == digest, 'replay source differs from sealed board source: ' + name)
    # The decoder's binding and this source seal must describe the same ELF.
    app = [row for row in seal.get('inputs', []) if row.get('name') == 'app']
    require(len(app) == 1, 'source seal must bind exactly one application ELF')
    binding = manifest.get('launch_identity', {})
    bound_elf = binding.get('elf_sha256')
    if bound_elf is None:
        bound_apps = [row for row in binding.get('inputs', []) if row.get('name') == 'app']
        bound_elf = bound_apps[0].get('sha256') if len(bound_apps) == 1 else None
    require(bound_elf == app[0]['sha256'], 'decoder binding and source seal have different application ELF')
    return {'status': 'MATCHED_RECORDED_ELF_SOURCE', 'source_root': str(source_root), 'source_seal': str(seal_path.resolve()),
            'source_seal_sha256': sha(seal_path), 'elf_sha256': app[0]['sha256'], 'sha256': actual}


def linux_path(path):
    path = Path(path).resolve()
    if os.name != 'nt':
        return str(path)
    require(len(path.drive) == 2 and path.drive[1] == ':', 'WSL replay compiler requires a local drive path')
    return '/mnt/' + path.drive[0].lower() + path.as_posix()[2:]


def run_replay(directory, output, sanitize=False, source_root=ROOT):
    output, source_root = Path(output), Path(source_root).resolve()
    executable = output / 'replay_reflection_record'
    prefix = ['wsl.exe', '--exec'] if os.name == 'nt' else []
    command = prefix + ['gcc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-UNDEBUG']
    if sanitize:
        command += ['-fsanitize=undefined', '-fno-sanitize-recover=all']
    command += ['-I' + linux_path(source_root / 'radar_runtime/src/processing'),
                linux_path(ROOT / 'tests/replay_reflection_record.c'),
                linux_path(source_root / 'radar_runtime/src/processing/radar_reflection.c'),
                '-lm', '-o', linux_path(executable)]
    json_write(output / 'compile_command.json', command)
    result = subprocess.run(command, capture_output=True, timeout=120)
    # WSL may prepend a UTF-16 Windows proxy notice to stderr, while gcc uses
    # UTF-8. Preserve original log bytes rather than decode with Windows GBK.
    (output / 'compile.log').write_bytes(result.stdout + result.stderr)
    require(result.returncode == 0, 'production reflection host build failed; see compile.log')
    command = prefix + [linux_path(executable), linux_path(Path(directory) / 'image.bin')]
    json_write(output / 'replay_command.json', command)
    result = subprocess.run(command, capture_output=True, timeout=120)
    (output / 'replay.jsonl').write_bytes(result.stdout)
    (output / 'replay_stderr.log').write_bytes(result.stderr)
    require(result.returncode == 0, 'production reflection replay failed; see replay_stderr.log')
    return [json.loads(line) for line in result.stdout.decode('utf-8').splitlines() if line.strip()]


def compare_replay(frames, replay):
    require(len(frames) == len(replay), 'replay frame count mismatch')
    failures = []
    compared = 0
    for expected, actual in zip(frames, replay):
        frame = expected['frame_id']
        if any(expected[key] != actual.get(key) for key in ('record_index', 'frame_id', 'timestamp_us')):
            failures.append({'frame_id': frame, 'field': 'frame_identity'})
            continue
        left, right = expected['reflections'], actual['reflections']
        if len(left) != len(right):
            failures.append({'frame_id': frame, 'field': 'reflection_count', 'recorded': len(left), 'replayed': len(right)})
            continue
        for a, b in zip(left, right):
            compared += 1
            for key in ('id', 'state', 'flags', 'age_ms'):
                if a[key] != b[key]:
                    failures.append({'frame_id': frame, 'id': a['id'], 'field': key, 'recorded': a[key], 'replayed': b[key]})
            for key in ('range_m', 'u', 'range_mad_m'):
                x, y = a[key], b[key]
                equal = x is None and y is None
                if x is not None and y is not None:
                    equal = math.isfinite(x) and math.isfinite(y) and abs(x - y) <= 1e-6
                if not equal:
                    failures.append({'frame_id': frame, 'id': a['id'], 'field': key, 'recorded': x, 'replayed': y})
    return {'status': 'PASS' if not failures else 'FAIL', 'frames': len(frames),
            'reflection_summaries_compared': compared, 'integer_fields': 'EXACT',
            'absolute_float_tolerance': 1e-6, 'id_seed': 'RECORDED_MANIFEST_SEED_NO_REMAP',
            'mismatch_count': len(failures), 'first_mismatches': failures[:32]}


def percentile(values, q):
    if not values:
        return None
    values = sorted(values)
    return values[max(0, math.ceil(len(values) * q) - 1)]


def metrics(frames, replay):
    """A fixed first 30s after 2s warm-up, not a post-hoc best interval."""
    output = {'window_policy': 'FIRST_30_SECONDS_AFTER_FIXED_2_SECOND_WARMUP',
              'stability_definition': 'same selected ID, STABLE and currently MEASURED; elapsed-time weighted',
              'physical_accuracy': 'NOT_PROVEN', 'semantic_classification': 'NOT_PROVEN'}
    processing = [row.get('diagnostics', {}).get('processing_us', 0) for row in frames]
    observed = [x for x in processing if isinstance(x, int) and x > 0]
    p95 = percentile(observed, .95)
    output['scene_cpu'] = {'status': ('PASS' if p95 <= 5000 else 'FAIL') if observed and len(observed) == len(frames) else 'NOT_PROVEN',
                           'measured_samples': len(observed), 'total_frames': len(frames), 'p95_us': p95,
                           'maximum_us': max(observed) if observed else None, 'limit_p95_us': 5000,
                           'clock_scope': 'RECORDED_A53_SCENE_CPU_SLICES_NOT_HOST_REPLAY_TIME'}
    last_seen, violations, expired, missing_samples = {}, [], 0, 0
    for row in replay:
        now = row['timestamp_us']
        current = {x['id']: x for x in row['reflections']}
        for ident, reflection in current.items():
            measured = bool(reflection['flags'] & 1)
            missing_samples += not measured
            previous = last_seen.get(ident)
            if reflection['last_observed_us'] > now or (measured and reflection['last_observed_us'] != now):
                violations.append({'frame_id': row['frame_id'], 'id': ident, 'reason': 'observation_timestamp'})
            if not measured and previous and reflection['last_observed_us'] != previous['last_observed_us']:
                violations.append({'frame_id': row['frame_id'], 'id': ident, 'reason': 'missing_measurement_renewed'})
            if now - reflection['last_observed_us'] > 500000:
                violations.append({'frame_id': row['frame_id'], 'id': ident, 'reason': 'expiry_exceeded'})
        expired += sum(ident not in current and now - value['last_observed_us'] > 500000 for ident, value in last_seen.items())
        last_seen = current
    missing_status = 'FAIL' if violations else ('PASS' if missing_samples and expired else 'NOT_PROVEN')
    output['missing_measurement_expiry'] = {'status': missing_status,
        'violations': violations[:32], 'unmeasured_summaries_observed': missing_samples, 'expiry_events_observed': expired,
        'note': 'No observed missing/expiry transition cannot prove that behavior from this sequence.',
        'physical_target_departure_validation': 'NOT_PROVEN'}
    if not frames:
        output['stable_30s'] = {'status': 'NOT_PROVEN', 'reason': 'NO_FRAMES'}
        return output
    begin = frames[0]['timestamp_us'] + 2000000
    end = begin + 30000000
    output['window_us'] = [begin, end]
    if frames[-1]['timestamp_us'] < end:
        output['stable_30s'] = {'status': 'NOT_PROVEN', 'reason': 'LESS_THAN_30S_AFTER_WARMUP'}
        return output
    in_window = [row for row in frames if begin <= row['timestamp_us'] <= end]
    first = next((row for row in frames if row['timestamp_us'] >= begin), None)
    candidates = [r['id'] for r in first['reflections'] if r['flags'] & 1] if first else []
    primary = min(candidates) if candidates else None
    supported = any_supported = covered = 0
    for a, b in zip(frames, frames[1:]):
        start, stop = max(begin, a['timestamp_us']), min(end, b['timestamp_us'])
        if stop <= start:
            continue
        dt = b['timestamp_us'] - a['timestamp_us']
        if dt <= 0 or dt > 500000 or not a['frame_flags'] & 4:
            continue
        span = stop - start
        covered += span
        stable = [r for r in a['reflections'] if r['state'] == 2 and r['flags'] & 1 and r['age_ms'] == 0]
        any_supported += span * bool(stable)
        supported += span * any(r['id'] == primary for r in stable)
    ratio = supported / 30000000
    output['stable_30s'] = {'status': 'NOT_PROVEN' if primary is None else ('PASS' if ratio >= .9 else 'FAIL'),
        'primary_id': primary, 'primary_selection': 'LOWEST_CURRENT_MEASURED_ID_AT_FIXED_WINDOW_START',
        'stable_time_fraction': ratio, 'any_stable_time_fraction': any_supported / 30000000,
        'coverage_time_fraction': covered / 30000000, 'required_fraction': .9,
        'denominator_us': 30000000, 'supported_us': supported,
        'note': 'Unobserved gaps contribute zero support; no best-window or best-ID search.'}
    by_id = {}
    for row in in_window:
        for r in row['reflections']:
            if r['flags'] & 1:
                by_id.setdefault(r['id'], []).append((row['timestamp_us'], r['range_m']))
    ranges = []
    for ident, samples in sorted(by_id.items()):
        values = [value for _, value in samples if value is not None and math.isfinite(value)]
        if not values:
            continue
        center = statistics.median(values)
        deviations = [abs(value - center) for value in values]
        ranges.append({'id': ident, 'samples': len(values), 'first_us': samples[0][0], 'last_us': samples[-1][0],
                       'range_median_m': center, 'range_mad_m': statistics.median(deviations),
                       'range_p95_absolute_deviation_m': percentile(deviations, .95),
                       'range_peak_to_peak_m': max(values) - min(values),
                       'meaning': 'REPEATABILITY_OF_REPORTED_LOS_NOT_ABSOLUTE_RANGE_ERROR'})
    output['per_id_range_repeatability'] = ranges
    id_sets = [tuple(sorted(r['id'] for r in row['reflections'] if r['flags'] & 1)) for row in frames]
    output['id_changes'] = {'scope': 'ALL_RECORDED_FRAMES', 'distinct_measured_ids': sorted({ident for group in id_sets for ident in group}),
        'measured_id_set_transitions': sum(a != b for a, b in zip(id_sets, id_sets[1:])),
        'identity_switch_ground_truth': 'NOT_PROVEN',
        'note': 'Admissions, disappearances and ambiguity can change the set; this is not a physical identity-switch rate.'}
    return output


def analyze(directory, output, kind, source_seal=None, sanitize=False, source_root=ROOT):
    directory, output = Path(directory).resolve(), Path(output).resolve()
    require(not output.exists(), 'output directory already exists; preserve earlier evidence')
    output.mkdir(parents=True)
    result = {'schema': 'M2_POINT_REFLECTION_REPLAY_V1', 'status': 'RUNNING', 'evidence_kind': kind.upper(),
              'physical_validation': 'NOT_PROVEN', 'complete_structural_replay': 'NOT_POSSIBLE_FROM_THREE_HEATMAPS'}
    try:
        manifest, frames = load_verified(directory)
        result['input'] = {'directory': str(directory), 'manifest_sha256': sha(directory / 'manifest.json'),
                           'frames_sha256': sha(directory / 'frames.jsonl'), 'image_sha256': manifest['image_sha256'],
                           'capture_epoch': manifest['epoch'], 'build_id': manifest['build_id'],
                           'reflection_id_seed': manifest['reflection_id_seed'], 'incomplete': manifest['incomplete'],
                           'record_error': manifest['error']}
        before = source_provenance(source_seal, manifest, kind, source_root)
        result['source'] = before
        result['replay_harness_sha256'] = sha(ROOT / 'tests/replay_reflection_record.c')
        replay = run_replay(directory, output, sanitize, source_root)
        require(before == source_provenance(source_seal, manifest, kind, source_root), 'production source changed during replay')
        require(result['replay_harness_sha256'] == sha(ROOT / 'tests/replay_reflection_record.c'), 'replay harness changed during execution')
        require(result['input']['image_sha256'] == sha(directory / 'image.bin') and
                result['input']['manifest_sha256'] == sha(directory / 'manifest.json') and
                result['input']['frames_sha256'] == sha(directory / 'frames.jsonl'), 'decoded input changed during replay')
        result['replay'] = compare_replay(frames, replay)
        result['metrics'] = metrics(frames, replay)
        result['map_reset_provenance'] = 'RECORDED_OUTER_SCENE_DECISIONS_APPLIED_NOT_INDEPENDENTLY_RECOMPUTED'
        checks = [result['replay']['status'], result['metrics']['stable_30s']['status'],
                  result['metrics']['scene_cpu']['status'], result['metrics']['missing_measurement_expiry']['status']]
        result['status'] = 'FAIL' if 'FAIL' in checks else ('NOT_PROVEN' if 'NOT_PROVEN' in checks or manifest['incomplete'] or manifest['error'] else 'PASS')
        if kind == 'test':
            result['status'] = 'TEST_' + result['status']
            result['real_scene_metrics'] = 'NOT_PROVEN_SYNTHETIC_INPUT'
    except Exception as error:
        result.update(status='TEST_FAIL' if kind == 'test' else 'FAIL', error=str(error))
    json_write(output / 'summary.json', result)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('decoded_dir', type=Path)
    parser.add_argument('--output-dir', required=True, type=Path)
    parser.add_argument('--kind', required=True, choices=('real', 'test'))
    parser.add_argument('--source-seal', type=Path)
    parser.add_argument('--source-root', type=Path, default=ROOT,
                        help='Production source root containing radar_runtime/src; must still match the recorded launch seal')
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    result = analyze(args.decoded_dir, args.output_dir, args.kind, args.source_seal, args.sanitize, args.source_root)
    print(json.dumps({'status': result['status'], 'summary': str(args.output_dir.resolve() / 'summary.json'),
                      'error': result.get('error')}, allow_nan=False))
    return 1 if result['status'] in ('FAIL', 'TEST_FAIL') else 0


if __name__ == '__main__':
    raise SystemExit(main())
