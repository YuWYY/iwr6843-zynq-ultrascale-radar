"""Fail-closed report qualification; native execution and download readiness are separate."""
from __future__ import annotations
import argparse
import json
import math
from pathlib import Path
import re
import sys
# -I is intentional under Vivado's embedded Python environment. Import only the
# sibling helper at this verified script location, not CWD/PYTHONPATH.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from build_identity import GateError, require, sha, read_json, write_new, require_synth, run_log_records, under

def number(value, label):
    try:
        result = float(value)
    except (TypeError, ValueError) as exc:
        raise GateError(f'Unparseable {label}: {value!r}') from exc
    require(math.isfinite(result), f'Non-finite {label}: {value}')
    return result

def nonnegative(value, label):
    result = number(value, label)
    require(result >= 0, f'Negative {label}: {result}')
    return result

def timing_summary(text):
    match = re.search(r'WNS\(ns\).*?\n\s*[- ]+\n\s*([^\n]+)', text, re.S)
    require(match is not None, 'Missing timing summary')
    values = match[1].split()
    require(len(values) == 12, f'Expected 12 timing summary fields, found {values}')
    values = [number(v, 'timing summary') for v in values]
    for i, label in ((0, 'WNS'), (4, 'WHS'), (8, 'WPWS')):
        nonnegative(values[i], label)
    for i in (1, 2, 5, 6, 9, 10):
        require(values[i] == 0, f'Timing failures at summary index {i}: {values[i]}')
    require(all(values[i] > 0 for i in (3, 7, 11)), 'Empty STA endpoint population')
    return dict(zip(('WNS','TNS','setup_failing','setup_total','WHS','THS','hold_failing','hold_total','WPWS','TPWS','pulse_failing','pulse_total'), values))

def route_summary(text):
    def count(label):
        m = re.search(re.escape(label) + r'\.*\s*:\s*(\d+)', text)
        require(m is not None, f'Missing route metric: {label}')
        return int(m[1])
    routable = count('# of routable nets')
    routed = count('# of fully routed nets')
    errors = count('# of nets with routing errors')
    require(routable > 0 and routable == routed and errors == 0, 'Route incomplete or empty')
    return {'routable': routable, 'fully_routed': routed, 'errors': errors}

def check_timing_counts(text):
    out = {name: int(count) for name, count in re.findall(r'checking (\w+) \((\d+)\)', text)}
    for name in ('no_clock','unconstrained_internal_endpoints','multiple_clock','generated_clocks','loops'):
        require(name in out and out[name] == 0, f'check_timing {name} missing/nonzero: {out.get(name)}')
    return out

def bus_skew(text, payloads):
    markers = list(re.finditer(r'^1\. Bus Skew Report Summary\s*$', text, re.M))
    require(markers, 'Missing bus-skew summary section')
    section = text[markers[-1].end():]
    section = re.split(r'^2\. Bus Skew Report Per Constraint\s*$', section, maxsplit=1, flags=re.M)[0]
    rows = []
    for block in re.split(r'(?m)^\s*(?=\d+\s+\d+\s+)', section):
        if not re.match(r'\s*\d+\s+\d+\s+', block):
            continue
        m = re.search(r'^\s*(Slow|Fast)\s+(\S+)\s+(\S+)\s+(\S+)\s*$', block, re.M)
        require(m is not None, f'Unparseable bus-skew row: {block[:120]}')
        requirement, actual, slack = (number(m[i], 'bus skew') for i in (2,3,4))
        require(requirement > 0 and actual >= 0, 'Invalid bus-skew requirement/actual')
        nonnegative(slack, 'bus-skew slack')
        require(abs(requirement - actual - slack) <= .003, 'Inconsistent bus-skew arithmetic')
        rows.append({'text': block, 'requirement': requirement, 'actual': actual, 'slack': slack})
    require(rows, 'Empty bus-skew constraint population')
    covered = []
    for payload in payloads:
        if not payload.get('bus_skew_required', 1):
            require(len(payload['source_bits']) == len(payload['destination_bits']) == 1, 'Only scalar payload can omit inter-bit skew')
            covered.append({'instance': payload['instance'], 'status': 'NOT_APPLICABLE_SINGLE_BIT'})
            continue
        matches = [r for r in rows if payload['source_stem'] in r['text'] and payload['destination_stem'] in r['text']]
        require(len(matches) == 1, f'Bus-skew coverage missing/ambiguous for {payload["instance"]}')
        require(abs(matches[0]['requirement'] - number(payload['requirement_ns'], 'payload requirement')) <= .001, 'Wrong effective bus-skew bound')
        covered.append({'instance': payload['instance'], **{k:v for k,v in matches[0].items() if k != 'text'}})
    return covered

def payload_metrics(metrics):
    require(metrics.get('object_checks') == 'PASS', 'Native object checks failed or missing')
    require(metrics.get('phy_same_net_pairs') == 3, 'Three PHY CLK/CLKDIV net matches required')
    require(metrics.get('registered_status_checks', 0) >= 5, 'Registered status source checks missing')
    require(metrics.get('reset_structure_checks', 0) > 0 and metrics.get('divider_clr_check') == 'PASS', 'Reset/CLR structural checks missing')
    shared_reset_metrics(metrics)
    rows = metrics.get('payloads')
    require(isinstance(rows, list) and rows, 'No held-payload channels checked')
    for row in rows:
        require(row['expected_destination_bits'] and row['destination_bits'] == row['expected_destination_bits'], f'Missing/extra destination bits: {row["instance"]}')
        require(row['source_bits'] == row['expected_source_bits'] and row['source_bits'], 'Missing/extra source bits')
        require(row['timed_endpoints'] == row['destination_count'] and row['timed_endpoints'] >= len(row['destination_bits']), 'Missing timed payload endpoint')
        require(number(row['requirement_ns'], 'requirement') > 0, 'Nonpositive requirement')
        nonnegative(row['worst_slack_ns'], 'max-delay slack')
        require(row.get('bus_skew_required', 1) in (0, 1), 'Invalid bus-skew applicability')
        require(row.get('bus_skew_required', 1) or len(row['source_bits']) == len(row['destination_bits']) == 1, 'Multi-bit skew cannot be omitted')
    for event in metrics.get('event_only_checks', []):
        require(event.get('payload_destination_registers') == 0 and event.get('functional_payload_consumers') == 0, 'Event-only payload has functional data consumers')
    return rows

def shared_reset_metrics(metrics):
    proof = metrics.get('shared_reset_ownership', {})
    require(proof.get('status') == 'PASS', 'Missing shared destination reset ownership proof')
    parents = proof.get('xpm_parents', [])
    require(len(parents) == 13 and len(set(parents)) == 13 and
            parents == proof.get('expected_xpm_parents') and
            all(p.endswith('/g_async.u_reset_sync') for p in parents),
            'Missing/extra/duplicated XPM reset parent')
    specs = {'common_sys':('u_common_sys_release','clk_pl_0',2),
             'common_rx':('u_rx_control_release','RADAR_RX75',11),
             'command_sys':('u_command_sys_release','clk_pl_0',1),
             'command_rx':('u_command_release','RADAR_RX75',2),
             'delay_sys':('u_phy/u_rx_epoch_sys_release','clk_pl_0',2),
             'delay_rx':('u_phy/u_rx_release','RADAR_RX75',4),
             'cal_sys':('u_phy/u_cal_epoch_sys_release','clk_pl_0',2),
             'cal_ref':('u_phy/u_cal_epoch_ref_release','clk_out1_radar_system_clk_wiz_phy_ref_0',3)}
    groups = proof.get('groups', [])
    require(len(groups) == len(specs) and {g['name'] for g in groups} == set(specs),
            'Missing/duplicate shared reset group')
    for group in groups:
        suffix, clock, minimum = specs[group['name']]
        owner = group.get('owner', '')
        require(owner.endswith('/'+suffix) and owner+'/g_async.u_reset_sync' in parents and
                group.get('clock') == clock, 'Wrong shared reset owner/clock')
        require(group.get('raw_chain_count') == 1 and group.get('raw_source_cells'),
                'Missing/duplicated raw asynchronous reset chain')
        expected = group.get('expected_consumers', [])
        consumers = group.get('consumers', [])
        actual = [c['pin'] for c in consumers]
        require(len(expected) >= minimum and len(set(expected)) == len(expected) and
                actual == expected, 'Missing/duplicate reset consumers')
        for consumer in consumers:
            sources = consumer.get('source_cells', [])
            require(sources and all(re.fullmatch(re.escape(owner)+r'/released_reg(?:_replica[^/]*)?', s)
                                    for s in sources), 'Consumer uses independent/wrong reset owner')
    local = proof.get('prequalified', [])
    require(len(local) == 2 and {p['owner'].split('/inst/',1)[-1] for p in local} ==
            {'u_fifo_release','u_phy/u_deser_release'}, 'Missing local HOLD reset proof')
    require(all(p.get('clock') == 'RADAR_RX75' and p.get('raw_xpm_count') == 0 and
                p.get('source_cells') for p in local), 'Local HOLD reintroduced a raw reset chain')
    return proof

def reset_metrics(metrics, stage='route'):
    require(stage in {'synth','route'}, 'Unknown reset verification stage')
    reset = metrics.get('phy_reset_timing', {})
    require(reset.get('status') == ('PASS' if stage == 'route' else 'PRE_ROUTE') and reset.get('shared_clock_gate') == 'PASS', 'PHY reset/clock topology proof missing or wrong stage')
    require(reset.get('clock_gate_clear_arc_audit') == 'PASS', 'Clock-stop propagation still needs native library review')
    arcs = reset.get('clock_clear_arcs', [])
    require(len(arcs) == 2 and len({a['buffer'] for a in arcs}) == 2, 'Two clock-buffer clear arcs required')
    clear_max = max(nonnegative(a['max_fall_ns'], 'clock clear arc') for a in arcs)
    require(clear_max > 0, 'Missing native clear delay')
    tree = nonnegative(reset.get('clock_tree_max_ns'), 'clock tree delay')
    leaves = reset.get('clock_leaf_delays', [])
    require(len(leaves) == 12 and len({r['pin'] for r in leaves}) == 12, 'Twelve primitive clock net delays required')
    require(abs(max(nonnegative(r['max_ns'],'leaf clock delay') for r in leaves)-tree) <= .001, 'Clock tree aggregate differs from native leaf delays')
    if stage == 'route':
        require(all(r.get('estimated') in (False,0) for r in leaves), 'Final clock propagation still estimated')
    guard = number(reset.get('clock_close_guard_ns'), 'clock close guard')
    margin = number(reset.get('clock_close_margin_ns'), 'clock close margin')
    require(margin > 0 and abs(margin - (guard-8-clear_max-tree)) <= .003, 'Clock-stop propagation not bounded by quiesce interval')
    require(number(reset.get('guard_after_propagation_ns'), 'reset propagation guard') > 40, 'Insufficient primitive-before-controller reset margin')
    expected = {'u_serdes_div':'DIVIDER_CLR', 'u_rx_div':'DIVIDER_CLR', 'delay0':'PRIMITIVE_RST', 'delay1':'PRIMITIVE_RST', 'delay2':'PRIMITIVE_RST', 'ser_fr':'PRIMITIVE_RST', 'ser_d0':'PRIMITIVE_RST', 'ser_d1':'PRIMITIVE_RST', 'idelayctrl':'CONTROLLER_RST'}
    endpoints = reset.get('endpoints', [])
    require(len(endpoints) == len(expected) and {row['label']:row['kind'] for row in endpoints} == expected, 'Reset endpoint population missing/duplicated')
    require(len({row['endpoint'] for row in endpoints}) == len(expected), 'Reset endpoints alias unexpectedly')
    for row in endpoints:
        paths = row.get('paths', [])
        require(len(paths) == 2 and {p['delay_type'] for p in paths} == {'max','min'}, 'Reset max/min path missing')
        require(row.get('source_cells'), 'Reset source register population empty')
        bound = number(row['bound_ns'], 'reset delay bound')
        require(0 < bound <= 8, 'Reset physical bound weakened')
        primitive = row['kind'] == 'PRIMITIVE_RST'
        checks = row.get('native_library_checks', [])
        if primitive:
            require(row.get('primitive_ref') in {'IDELAYE3','ISERDESE3'} and checks == [] and
                    row.get('reset_propagation_arcs',0) > 0, 'Primitive no-check library proof missing')
        else:
            require({'recovery','removal'}.issubset(checks), 'Native reset R/R checks missing')
        for path in paths:
            require(path.get('startpoint') and path.get('endpoint') == row['endpoint'], 'Reset path endpoint mismatch')
            if primitive and path['delay_type'] == 'min':
                require(path.get('native_check') == 'NO_LIBRARY_CHECK' and path.get('slack_ns') is None and
                        path.get('requirement_ns') is None and path.get('raw_slack') == '' and
                        path.get('raw_requirement') == '', 'Invented or unreviewed primitive minimum slack')
            else:
                require(path.get('native_check') == 'NATIVE', 'Missing actual native check')
                if stage == 'route': nonnegative(path['slack_ns'], 'reset max/min slack')
                else: number(path['slack_ns'], 'pre-route reset slack')
                number(path['requirement_ns'], 'reset timing requirement')
            require(nonnegative(path['datapath_ns'], 'reset propagation') <= bound, 'Reset propagation exceeds bound')
            number(path['endpoint_clock_delay_ns'], 'reset endpoint clock latency')
            require(not re.search(r'false|datapath.only', path.get('exception',''), re.I), 'Reset recovery/removal is excepted')
    return reset

def cdc_critical(text):
    totals = {key: int(count) for key, count in re.findall(r'^(CDC-\d+)\s+Critical\s+(\d+)\s+', text, re.M)}
    require(re.search(r'^ID\s+Severity\s+Count', text, re.M), 'Missing CDC summary')
    records = []
    src_clock = dst_clock = ''
    for line in text.splitlines():
        if line.startswith('Source Clock: '): src_clock = line.split(': ',1)[1]
        if line.startswith('Destination Clock: '): dst_clock = line.split(': ',1)[1]
        match = re.match(r'^\s*\d+\s+(CDC-\d+)\s+Critical\s+.*?\s+(\S+)\s+(\S+)\s*$', line)
        if match:
            require(src_clock and dst_clock, 'CDC path without clock context')
            records.append({'rule': match[1], 'source_clock': src_clock, 'destination_clock': dst_clock, 'source': match[2], 'destination': match[3]})
    actual = {key: sum(r['rule'] == key for r in records) for key in totals}
    require(actual == totals and len(records) == sum(totals.values()), 'CDC Critical details do not match summary')
    return records

def drc_critical(text):
    require('REPORT SUMMARY' in text and 'REPORT DETAILS' in text, 'Missing DRC report sections')
    records = []
    for match in re.finditer(r'^([A-Z]+-\d+)#(\d+) (Critical Warning|Error)\s*\n(.*?)(?=^[A-Z]+-\d+#\d+ |\Z)', text, re.M | re.S):
        rule, _, severity, detail = match.groups()
        records.append({'rule': rule, 'severity': severity, 'detail_sha256': __import__('hashlib').sha256(detail.strip().encode()).hexdigest()})
    totals = sum(int(count) for _, count in re.findall(r'^\|\s*[A-Z]+-\d+\s*\|\s*(Critical Warning|Error)\s*\|[^\n]+?\|\s*(\d+)\s*\|', text, re.M))
    require(len(records) == totals, 'DRC Critical/Error details do not match summary')
    require(not any(r['rule'] == 'REQP-1742' or r['severity'] == 'Error' for r in records), 'PHY REQP-1742 or DRC Error remains')
    return records

def accepted_records(records, decisions, root, kind):
    remaining = list(records)
    for item in decisions:
        require(item.get('decision') == 'reviewed_accept' and len(item.get('reason','')) >= 32, 'Unreviewed/empty Critical exception')
        evidence = item.get('evidence', {})
        require(evidence, 'Critical exception has no evidence')
        for name, expected in evidence.items():
            p = (root/name).resolve()
            require(p.is_file() and sha(p) == expected, f'Critical evidence stale/missing: {name}')
        require(item['object'] in remaining, f'Stale/nonexact {kind} exception')
        remaining.remove(item['object'])
    return remaining

def require_functional_cdc_clean(records):
    capture = 'radar_capture_bd_0/inst/'
    require(not any(item['rule'] == 'CDC-10' and
                    any(capture in item.get(field, '') for field in ('source', 'destination'))
                    for item in records),
            'Functional CDC-10 remains in capture RTL; exact waivers cannot accept it')

def native_log_records(root, report_dir):
    audit = read_json(report_dir/'native_run_logs.json')
    rows = audit.get('logs', [])
    require(audit.get('stage') == 'route' and len(rows) == 2 and
            {r['run'] for r in rows} == {'synth_1','impl_1'}, 'Missing actual synth/impl run logs')
    records = []
    for row in rows:
        require(row['copy'] == row['run']+'_runme.log' and row['path'].startswith('project/'), 'Invalid run log provenance')
        p = (root/row['path'][8:]).resolve()
        copy = report_dir/row['copy']
        require(under(p,root) and p.name == 'runme.log' and p.parent.name == row['run'], 'Run log outside actual run directory')
        require(p.is_file() and copy.is_file() and sha(p) == row['sha256'] == sha(copy), 'Actual/copied run log hash changed')
        actual = run_log_records(copy.read_text(encoding='utf-8-sig', errors='strict'), row['run'])
        require(actual == row['records'], 'Run log exact message inventory changed')
        require(not any(r['severity'] == 'ERROR' or 'REQP-1742' in r['rule'] for r in actual), 'Native ERROR or PHY REQP-1742 cannot be waived')
        records.extend(actual)
    return records

def evaluate(root, report_dir, policy):
    def text(name):
        return (report_dir/name).read_text(encoding='utf-8-sig', errors='strict')
    metrics = read_json(report_dir/'constraint_metrics.json')
    require(metrics.get('verification_stage') == 'route', 'Pre-route observations cannot qualify release')
    payloads = payload_metrics(metrics)
    result = {'phy_reset_timing': reset_metrics(metrics), 'payload_bus_skew': bus_skew(text('bus_skew.rpt'), payloads),
              'timing': timing_summary(text('timing.rpt')), 'routing': route_summary(text('route_status.rpt')),
              'check_timing': check_timing_counts(text('check_timing.rpt'))}
    cdc = cdc_critical(text('cdc.rpt'))
    require_functional_cdc_clean(cdc)
    drc = drc_critical(text('drc.rpt'))
    native_logs = native_log_records(root, report_dir)
    result['unexplained_cdc'] = accepted_records(cdc, policy.get('cdc_accepted', []), root, 'CDC')
    result['unexplained_drc'] = accepted_records(drc, policy.get('drc_accepted', []), root, 'DRC')
    result['unexplained_native_log'] = accepted_records(native_logs, policy.get('native_log_accepted', []), root, 'native log')
    result['PHY_RULE'] = 'PASS'
    result['CDC_SIGNOFF'] = 'PASS' if not result['unexplained_cdc'] else 'BLOCKED'
    result['DRC_SIGNOFF'] = 'PASS' if not result['unexplained_drc'] else 'BLOCKED'
    result['NATIVE_LOG_SIGNOFF'] = 'PASS' if not result['unexplained_native_log'] else 'BLOCKED'
    # Old policy strings are not evidence. R07 alone owns physical/source
    # qualification and must check both the contract and sealed native reports.
    require(policy.get('source_timing', 'PENDING') == 'PENDING' and policy.get('board_electrical', 'PENDING') == 'PENDING',
            'External signoff declarations belong in the evidence-bound R07 contract, not release_policy')
    from source_timing import qualify_r07
    external = qualify_r07(root, report_dir)
    result['R07'] = external
    result['SOURCE_TIMING'] = external['SOURCE_TIMING']
    result['BOARD_ELECTRICAL'] = external['BOARD_ELECTRICAL']
    result['internal_status'] = 'PASS' if result['CDC_SIGNOFF'] == result['DRC_SIGNOFF'] == result['NATIVE_LOG_SIGNOFF'] == 'PASS' else 'BLOCKED'
    result['BITSTREAM_ELIGIBLE'] = 'YES' if result['internal_status'] == 'PASS' and external['eligible'] is True else 'NO'
    # A qualified routed design is permission to run bitgen, not an exported
    # candidate. Only hardware_export's actual bit/XSA verification can say YES.
    result['DOWNLOAD_CANDIDATE'] = 'NO'
    result['BOARD_TEST'] = 'NOT_RUN'
    result['report_sha256'] = {p.name: sha(p) for p in sorted(report_dir.iterdir()) if p.is_file()}
    return result

def verify_stage(status, receipt):
    require(status.get('state') == 'NATIVE_COMPLETE' and status.get('stage') == 'route', 'Current route attempt not complete (old PASS forbidden)')
    require(receipt.get('status') == 'NATIVE_COMPLETE' and receipt['build_id'] == status['build_id'], 'Route receipt/build mismatch')

def qualify_current(args):
    """Recheck actual inputs/runs/report bytes; a previous PASS file is insufficient."""
    status = read_json(args.stage_status)
    receipt = read_json(args.route_receipt)
    verify_stage(status, receipt)
    require_synth(args.root, args.native, args.simulation, args.manifest, args.synth_dcp, args.synth_receipt)
    require(receipt['manifest_sha256'] == sha(args.manifest) and receipt['synth_receipt_sha256'] == sha(args.synth_receipt), 'Route lineage is stale')
    routed = receipt['actual_run_routed_dcp']
    require(routed['path'].startswith('project/'), 'Routed DCP outside project')
    routed_path = (args.root / routed['path'][8:]).resolve()
    require(under(routed_path, args.root) and sha(routed_path) == routed['sha256'], 'Actual routed DCP changed')
    reports = {p.name: sha(p) for p in args.reports.iterdir() if p.is_file()}
    require(reports == receipt['reports'], 'Native report membership/hash changed after route sealing')
    result = evaluate(args.root.resolve(), args.reports, read_json(args.policy))
    result.update({'SIMULATION': 'PASS', 'NATIVE_ROUTE_EXECUTION': 'PASS', 'build_id': receipt['build_id'], 'route_receipt_sha256': sha(args.route_receipt)})
    return result

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--root', type=Path, required=True)
    ap.add_argument('--reports', type=Path, required=True)
    ap.add_argument('--route-receipt', type=Path, required=True)
    ap.add_argument('--stage-status', type=Path, required=True)
    ap.add_argument('--policy', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--native', type=Path, required=True)
    ap.add_argument('--manifest', type=Path, required=True)
    ap.add_argument('--simulation', type=Path, required=True)
    ap.add_argument('--synth-receipt', type=Path, required=True)
    ap.add_argument('--synth-dcp', type=Path, required=True)
    args = ap.parse_args()
    result = {'SIMULATION': 'NOT_RUN', 'NATIVE_ROUTE_EXECUTION': 'FAIL', 'DOWNLOAD_CANDIDATE': 'NO', 'BOARD_TEST': 'NOT_RUN'}
    try:
        result.update(qualify_current(args))
        write_new(args.output, result)
        print(json.dumps(result, indent=2))
        return 0 if result['internal_status'] == 'PASS' else 2
    except (GateError, OSError, ValueError, KeyError, TypeError) as error:
        result['internal_status'] = 'FAIL'; result['error'] = str(error)
        if not args.output.exists(): write_new(args.output, result)
        print(f'RELEASE_GATE_FAIL: {error}', file=sys.stderr)
        return 1

if __name__ == '__main__':
    raise SystemExit(main())
