"""Closed-world native build identity. Never modifies design inputs or deletes files."""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys
from datetime import datetime, timezone

SCHEMA = 1
TOOL_ROOT = Path('C:/Xilinx/2025.1')
REUSED_HELPERS = ('scripts/m2_real/package_review.py',
                  'scripts/m2_real/reopen_package.tcl', 'scripts/m2_real/common.tcl')
SMARTCONNECT_RESETS = (('radar_system_smartconnect_0_2', 'bd_888c'),
                       ('radar_system_smartconnect_0_3', 'bd_484d'))

class GateError(RuntimeError):
    pass

def require(ok, message):
    if not ok:
        raise GateError(message)

def sha(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as f:
        for data in iter(lambda: f.read(1024 * 1024), b''):
            h.update(data)
    return h.hexdigest()

def read_json(path):
    return json.loads(Path(path).read_text(encoding='utf-8-sig'))

def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'), ensure_ascii=False, allow_nan=False).encode('utf-8')

def digest(value):
    return hashlib.sha256(canonical(value)).hexdigest()

def under(path, root):
    try:
        Path(path).resolve().relative_to(Path(root).resolve())
        return True
    except ValueError:
        return False

def write_new(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open('x', encoding='utf-8', newline='\n') as f:
        json.dump(value, f, ensure_ascii=False, indent=2, allow_nan=False)
        f.write('\n')

def logical(path, root):
    path = Path(path).resolve()
    require('downloads' not in (p.lower() for p in path.parts), f'Downloads input: {path}')
    if under(path, root):
        return 'project/' + path.relative_to(root).as_posix()
    if under(path, TOOL_ROOT):
        return 'tool/' + path.relative_to(TOOL_ROOT).as_posix()
    raise GateError(f'Unapproved external input: {path}')

def normalize(value, root):
    if isinstance(value, str):
        return value.replace('\\', '/').replace(root.as_posix(), '$ROOT').replace(TOOL_ROOT.as_posix(), '$TOOL')
    if isinstance(value, list):
        return [normalize(v, root) for v in value]
    if isinstance(value, dict):
        return {k: normalize(v, root) for k, v in value.items()}
    return value

def check_sim(root, gate_path):
    gate_path = Path(gate_path).resolve()
    gate = read_json(gate_path)
    require(gate.get('status') == 'PASS', 'Simulation gate is not PASS')
    require(isinstance(gate.get('tests'), list) and gate['tests'], 'Empty simulation test set')
    definition_path = root / 'scripts/m2_closure/required_tests.json'
    definition = read_json(definition_path)
    required = set(definition['tests'])
    for tb, phases in definition.get('phase_tests', {}).items():
        required.update(tb + '_' + phase for phase in phases)
    require(required and required.issubset(gate['tests']), 'Required simulation tests omitted')
    suite = (root / gate['suite']).resolve()
    require(under(suite, root), 'Simulation suite escapes project')
    state = read_json(suite/'STATUS.json')
    require(state.get('state') == 'PASS' and state.get('required_suite') is True, 'Simulation suite is failed/incomplete/diagnostic-only')
    require(sorted(state['tests']) == sorted(gate['tests']) and all(state['tests'].get(t) == 'PASS' for t in required), 'Simulation STATUS/gate mismatch')
    require(read_json(suite/'input_hashes.json') == gate['input_hashes'], 'Suite input manifest differs from gate')
    for field in ('input_hashes', 'log_hashes'):
        require(isinstance(gate.get(field), dict) and gate[field], f'Empty simulation {field}')
        for name, expected in gate[field].items():
            p = (root / name).resolve()
            require(under(p, root) and p.is_file(), f'Missing/escaping simulation evidence: {name}')
            require(sha(p) == expected, f'Stale/tampered simulation {field}: {name}')
    require(definition_path.relative_to(root).as_posix() in gate['input_hashes'], 'Required test definition not bound to simulation')
    actual_logs = {p.relative_to(root).as_posix() for p in suite.glob('*.log')}
    require(actual_logs == set(gate['log_hashes']), 'Simulation log membership differs from gate')
    for test in required:
        log = suite / ('run_' + test + '.log')
        require(log.relative_to(root).as_posix() in gate['log_hashes'], f'Missing actual simulation log: {test}')
        content = log.read_text(encoding='utf-8-sig', errors='strict')
        marker = next((tb for tb in definition['tests'] if test == tb or test.startswith(tb+'_PHASE')), test)
        require(re.search(r'PASS\s+' + re.escape(marker) + r'\b', content), f'Missing actual PASS marker: {test}')
        require(not re.search(r'(?im)^\s*(?:ERROR:|FATAL|Fatal:)', content), f'Simulation error/fatal: {test}')
    production = {p.relative_to(root).as_posix() for p in (root / 'rtl').rglob('*')
                  if p.is_file() and p.suffix.lower() in {'.v', '.sv', '.vh', '.svh'}}
    require(production and production.issubset(gate['input_hashes']), 'Simulation does not cover every production RTL file')
    require({'sim/' + tb + '.sv' for tb in definition['tests']}.issubset(gate['input_hashes']), 'Simulation does not bind every required testbench')
    return {'path': logical(gate_path, root), 'sha256': sha(gate_path), 'tests': sorted(gate['tests'])}

def smartconnect_resets(root, native):
    """Reject regenerated SmartConnect children that would stay in reset.

    Parent IP settings can remain unchanged while native generation loses the
    propagated polarity of the tied-high external reset. Check actual sources.
    """
    root = Path(root).resolve()
    active = [Path(row['path']).resolve() for row in native['files']]
    compiled = [Path(p).resolve() for p in native['compile_order']['synthesis_sources']]
    expected, results = [], []
    for instance, bd in SMARTCONNECT_RESETS:
        base = root / 'Radar_XCZU2CG_M2.gen/sources_1/bd/radar_system/ip' / instance / 'bd_0'
        xci = base / 'ip/ip_1' / (bd + '_psr_aclk_0.xci')
        vhdl = base / 'ip/ip_1/synth' / (bd + '_psr_aclk_0.vhd')
        parent = base / 'synth' / (bd + '.v')
        constant = base / 'ip/ip_0/synth' / (bd + '_one_0.v')
        expected.append(xci.resolve())
        for path in (xci, vhdl, parent, constant):
            require(active.count(path.resolve()) == 1 and path.is_file(),
                    f'SmartConnect reset active input missing/duplicated: {path}')
        for path in (vhdl, parent, constant):
            require(compiled.count(path.resolve()) == 1,
                    f'SmartConnect reset synthesis input missing/duplicated: {path}')
        data = read_json(xci)['ip_inst']['parameters']
        for section in ('component_parameters', 'model_parameters'):
            for name in ('C_EXT_RESET_HIGH', 'C_AUX_RESET_HIGH'):
                values = data.get(section, {}).get(name, [])
                require(len(values) == 1 and values[0].get('value') == '0',
                        f'SmartConnect reset polarity: {instance} {section}.{name} must be 0')
        body = re.sub(r'--[^\n]*', '', vhdl.read_text(encoding='utf-8-sig'))
        for name in ('C_EXT_RESET_HIGH', 'C_AUX_RESET_HIGH'):
            values = re.findall(r'\b' + name + r"\s*=>\s*'([01])'", body, re.I)
            require(values == ['0'], f'SmartConnect synthesized reset polarity: {instance} {name} must be 0')

        def verilog(path):
            text = path.read_text(encoding='utf-8-sig')
            return re.sub(r'/\*.*?\*/|//[^\n]*', '', text, flags=re.S)

        text = verilog(parent)
        maps = re.findall(r'\b' + re.escape(bd + '_psr_aclk_0') +
                          r'\s+psr_aclk\s*\((.*?)\)\s*;', text, re.S)
        require(len(maps) == 1 and re.findall(r'\.ext_reset_in\s*\(\s*(\w+)\s*\)', maps[0]) == ['one_dout'],
                f'SmartConnect external reset is not uniquely tied to one_dout: {instance}')
        require(re.findall(r'\.aux_reset_in\s*\(\s*(\w+)\s*\)', maps[0]) == ['aresetn'],
                f'SmartConnect auxiliary reset is not aresetn: {instance}')
        maps = re.findall(r'\b' + re.escape(bd + '_one_0') +
                          r'\s+one\s*\((.*?)\)\s*;', text, re.S)
        require(len(maps) == 1 and re.findall(r'\.dout\s*\(\s*(\w+)\s*\)', maps[0]) == ['one_dout'],
                f'SmartConnect one_dout driver differs: {instance}')
        values = re.findall(r'\.CONST_VAL\s*\(\s*([^()\s]+)\s*\)', verilog(constant), re.I)
        require(len(values) == 1 and re.fullmatch(r"1'[hbd]1", values[0], re.I),
                f'SmartConnect external reset constant is not one bit high: {instance}')
        results.append({'instance': instance, 'ext_reset_high': 0, 'aux_reset_high': 0,
                        'external_tie': 1, 'inputs': [logical(p, root) for p in (xci, vhdl, parent, constant)]})
    actual = [p for p in active if p.name.endswith('_psr_aclk_0.xci')]
    require(sorted(actual) == sorted(expected), 'SmartConnect generated reset instance set differs from the two expected instances')
    return results


def snapshot(root, native_path, sim_path):
    root = Path(root).resolve()
    native = read_json(native_path)
    require(native.get('schema') == SCHEMA, 'Unknown native inventory schema')
    require(Path(native['project_root']).resolve() == root, 'Native inventory belongs to another project')
    require(native['project']['PART'] == 'xczu2cg-sfvc784-1-e', 'Wrong part')
    require(native['filesets']['sources_1']['TOP'] == 'radar_m2_top', 'Wrong physical top')
    require(native['tool_version'] == '2025.1', 'Wrong Vivado version')
    require(native.get('files'), 'Native active file inventory is empty')
    files = {}
    entries = {}
    for row in native['files']:
        p = Path(row['path']).resolve()
        name = logical(p, root)
        require(p.is_file(), f'Active file missing: {p}')
        require(name not in entries, f'Duplicate native input: {p}')
        require(p.suffix.lower() != '.dcp', f'Active input DCP prohibited in global build: {p}')
        entries[name] = normalize(row['properties'], root)
        files[name] = sha(p)
    require(any(name.endswith('.xdc') for name in files), 'No active XDC inputs')
    order = native.get('compile_order', {})
    require(set(order) == {'synthesis_sources', 'synthesis_constraints', 'implementation_constraints', 'implementation_inputs'},
            'RTL native synthesis/implementation inventory is incomplete')
    for kind, paths in order.items():
        require(isinstance(paths, list) and paths and len(paths) == len(set(paths)), f'Empty/duplicated {kind} inventory')
        names = [logical(Path(p).resolve(), root) for p in paths]
        require(all(name in entries for name in names), f'{kind} references an omitted/disabled input')
        if kind.endswith('_constraints'):
            require(all(name.lower().endswith('.xdc') for name in names), f'Non-XDC in {kind}')
    implementation = {name for name, props in entries.items() if str(props.get('USED_IN_IMPLEMENTATION', '')).lower() in {'1','true','yes'}}
    require(implementation == {logical(Path(p).resolve(), root) for p in order['implementation_inputs']},
            'Enabled implementation file membership differs from native inventory')
    reset_evidence = smartconnect_resets(root, native)
    # Inventory membership is part of the identity, so adding/removing a helper
    # or final constraint cannot be missed by checking only old manifest keys.
    recipes = sorted(p for p in (root / 'scripts/m2_closure').rglob('*')
                     if p.is_file() and p.suffix.lower() in {'.py', '.tcl', '.json'})
    require(recipes, 'No closure build scripts')
    for p in recipes:
        files[logical(p, root)] = sha(p)
    for name in REUSED_HELPERS:
        p = root / name
        require(p.is_file(), f'Required reused helper missing: {name}')
        files[logical(p, root)] = sha(p)
    # Explicit source inventory also detects an unregistered production module.
    production = sorted(p for p in (root / 'rtl').rglob('*') if p.is_file() and p.suffix.lower() in {'.v', '.sv', '.vh', '.svh'})
    for p in production:
        require(logical(p, root) in files, f'Production RTL absent from native active files: {p}')
    settings = {k: v for k, v in native.items() if k not in {'files', 'project_root'}}
    return {'schema': SCHEMA, 'settings': normalize(settings, root),
            'active_files': entries, 'files': files, 'simulation': check_sim(root, sim_path),
            'smartconnect_resets': reset_evidence}

def record(root, native, sim, output, build_id):
    require(re.fullmatch(r'[A-Za-z0-9_-]+', build_id) is not None, 'Unsafe build_id')
    body = snapshot(root, native, sim)
    value = {'schema': SCHEMA, 'build_id': build_id, 'created_utc': datetime.now(timezone.utc).isoformat(),
             'identity_sha256': digest(body), 'identity': body}
    write_new(output, value)
    return value

def check(root, native, sim, manifest):
    recorded = read_json(manifest)
    require(recorded['identity_sha256'] == digest(recorded['identity']), 'Manifest self-identity mismatch')
    current = snapshot(root, native, sim)
    require(current == recorded['identity'], 'Current native active inputs/settings/scripts/simulation differ from synth manifest')
    return recorded

def dcp_info(root, native, path):
    inv = read_json(native)
    expected = Path(inv['runs']['synth_1']['DIRECTORY']) / (inv['filesets']['sources_1']['TOP'] + '.dcp')
    path = Path(path).resolve()
    require(path == expected.resolve(), f'Not the actual synth run DCP: expected {expected}, got {path}')
    require(under(path, root) and path.is_file() and path.stat().st_size > 0, 'Missing/empty actual synthesis DCP')
    return {'path': logical(path, Path(root).resolve()), 'sha256': sha(path), 'size': path.stat().st_size}

def run_log_records(text, run):
    """Keep exact messages/objects; repeated identical warnings remain separate."""
    records, occurrences = [], {}
    lines = text.splitlines()
    for i, line in enumerate(lines):
        match = re.match(r'^\s*(CRITICAL WARNING|ERROR):\s*(?:\[([^\]]+)\])?', line)
        if not match:
            continue
        message = line.strip()
        for tail in lines[i+1:]:
            if not tail[:1].isspace() or not tail.strip():
                break
            if re.match(r'^\s*(?:INFO|WARNING|CRITICAL WARNING|ERROR):', tail):
                break
            message += '\n' + tail.rstrip()
        key = (match[1], match[2] or '', message)
        occurrences[key] = occurrences.get(key, 0) + 1
        records.append({'run': run, 'severity': key[0], 'rule': key[1],
                        'text': message, 'occurrence': occurrences[key]})
    return records

def run_log_info(root, native, run):
    require(run in {'synth_1', 'impl_1'}, 'Unknown native run log')
    p = (Path(read_json(native)['runs'][run]['DIRECTORY']) / 'runme.log').resolve()
    require(under(p, root) and p.is_file() and p.stat().st_size > 0, f'Missing/empty actual run log: {p}')
    data = p.read_bytes()
    return {'run': run, 'path': logical(p, Path(root).resolve()),
            'sha256': hashlib.sha256(data).hexdigest(), 'size': len(data),
            'records': run_log_records(data.decode('utf-8-sig', errors='strict'), run)}

def capture_run_logs(root, native, reports, stage):
    require(stage in {'synth', 'route'}, 'Unknown run-log stage')
    rows = []
    for run in (['synth_1'] if stage == 'synth' else ['synth_1', 'impl_1']):
        row = run_log_info(root, native, run)
        source = Path(root) / row['path'][8:]
        target = Path(reports) / (run + '_runme.log')
        with target.open('xb') as f:
            f.write(source.read_bytes())
        require(sha(target) == row['sha256'] == sha(source), f'Run log changed while sealing: {run}')
        rows.append({**row, 'copy': target.name})
    value = {'schema': SCHEMA, 'stage': stage, 'logs': rows}
    write_new(Path(reports)/'native_run_logs.json', value)
    return value

def seal_synth(root, native, sim, manifest, dcp, output):
    m = check(root, native, sim, manifest)
    log = run_log_info(root, native, 'synth_1')
    require(not any(r['severity'] == 'ERROR' for r in log['records']), 'Actual synthesis log contains ERROR')
    value = {'schema': SCHEMA, 'stage': 'synth', 'status': 'PASS', 'build_id': m['build_id'],
             'manifest_sha256': sha(manifest), 'identity_sha256': m['identity_sha256'],
             'actual_run_dcp': dcp_info(root, native, dcp), 'actual_run_log': log}
    write_new(output, value)
    return value

def require_synth(root, native, sim, manifest, dcp, receipt):
    m = check(root, native, sim, manifest)
    r = read_json(receipt)
    require(r.get('status') == 'PASS' and r.get('stage') == 'synth', 'Synthesis receipt is not PASS')
    require(r['build_id'] == m['build_id'] and r['manifest_sha256'] == sha(manifest)
            and r['identity_sha256'] == m['identity_sha256'], 'Synthesis receipt/manifest identity mismatch')
    require(r['actual_run_dcp'] == dcp_info(root, native, dcp), 'Actual synthesis DCP changed or is stale')
    require(r.get('actual_run_log') == run_log_info(root, native, 'synth_1'), 'Actual synthesis log changed or is stale')
    return r

def seal_route(root, native, sim, manifest, dcp, synth_receipt, output, routed_dcp, reports):
    r = require_synth(root, native, sim, manifest, dcp, synth_receipt)
    inv = read_json(native)
    expected = Path(inv['runs']['impl_1']['DIRECTORY']) / (inv['filesets']['sources_1']['TOP'] + '_routed.dcp')
    actual = Path(routed_dcp).resolve()
    require(actual == expected.resolve() and actual.is_file(), 'Not the actual implementation run routed DCP')
    logs = [run_log_info(root, native, run) for run in ('synth_1', 'impl_1')]
    captured = read_json(Path(reports)/'native_run_logs.json')
    require(captured.get('stage') == 'route' and
            [{k:v for k,v in row.items() if k != 'copy'} for row in captured['logs']] == logs,
            'Captured native run logs differ from actual completed runs')
    value = {'schema': SCHEMA, 'stage': 'route', 'status': 'NATIVE_COMPLETE', 'build_id': r['build_id'],
             'manifest_sha256': sha(manifest), 'identity_sha256': r['identity_sha256'],
             'synth_receipt_sha256': sha(synth_receipt), 'source_synth_dcp': r['actual_run_dcp'],
             'actual_run_routed_dcp': {'path': logical(actual, Path(root).resolve()), 'sha256': sha(actual), 'size': actual.stat().st_size},
             'actual_run_logs': logs,
             'reports': {p.name: sha(p) for p in sorted(Path(reports).iterdir()) if p.is_file()}}
    require(value['reports'], 'No native reports to seal')
    write_new(output, value)
    return value

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('mode', choices=['record', 'check', 'seal-synth', 'require-synth', 'seal-route', 'capture-logs'])
    ap.add_argument('--root', type=Path, required=True)
    ap.add_argument('--native', type=Path, required=True)
    ap.add_argument('--simulation', type=Path, required=True)
    ap.add_argument('--manifest', type=Path, required=True)
    ap.add_argument('--build-id')
    ap.add_argument('--dcp', type=Path)
    ap.add_argument('--receipt', type=Path)
    ap.add_argument('--output', type=Path)
    ap.add_argument('--routed-dcp', type=Path)
    ap.add_argument('--reports', type=Path)
    ap.add_argument('--stage', choices=['synth','route'])
    args = ap.parse_args()
    try:
        if args.mode == 'capture-logs':
            require(args.reports and args.stage, '--reports and --stage required')
            result = capture_run_logs(args.root, args.native, args.reports, args.stage)
        elif args.mode == 'record':
            require(args.build_id, '--build-id required')
            result = record(args.root, args.native, args.simulation, args.manifest, args.build_id)
        elif args.mode == 'check':
            result = check(args.root, args.native, args.simulation, args.manifest)
        elif args.mode == 'seal-route':
            require(args.dcp and args.receipt and args.output and args.routed_dcp and args.reports, 'Route receipt arguments required')
            result = seal_route(args.root, args.native, args.simulation, args.manifest, args.dcp, args.receipt, args.output, args.routed_dcp, args.reports)
        else:
            require(args.dcp and args.receipt, '--dcp and --receipt required')
            fn = seal_synth if args.mode == 'seal-synth' else require_synth
            result = fn(args.root, args.native, args.simulation, args.manifest, args.dcp, args.receipt)
        print(json.dumps({'status': 'PASS', 'mode': args.mode, 'build_id': result.get('build_id')}, indent=2))
        return 0
    except (GateError, OSError, ValueError, KeyError, TypeError) as error:
        print(f'BUILD_IDENTITY_FAIL: {error}', file=sys.stderr)
        return 1

if __name__ == '__main__':
    raise SystemExit(main())
