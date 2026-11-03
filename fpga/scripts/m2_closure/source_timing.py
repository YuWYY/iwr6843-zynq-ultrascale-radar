"""R07 source/board evidence and dual-edge XDC checks. Never changes the project.

Human-reviewed, hash-bound evidence is an input, not something this tool infers
from plausible numbers. The native report proves applied constraints and paths;
neither a JSON status nor XDC alone proves physical voltage or firmware identity.
"""
from __future__ import annotations
import argparse
from decimal import Decimal, ROUND_FLOOR, ROUND_CEILING
import hashlib
import json
import math
from pathlib import Path
import re
import sys

PORTS = ('radar_data0_p', 'radar_data1_p', 'radar_frclk_p')
EDGES = ('rise', 'fall')
CONTRACT = 'scripts/m2_closure/R07_contract.json'
XDC = 'constraints/radar_source_timing.xdc'
BASE = 'u_system/radar_system_i/radar_capture_bd_0/inst/u_phy/'
SERDES = dict(zip(PORTS, ('u_d0_ser', 'u_d1_ser', 'u_fr_ser')))
LANES = dict(zip(PORTS, (1, 2, 0)))

class TimingError(ValueError):
    pass

def need(condition, message):
    if not condition:
        raise TimingError(message)

def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def read(path):
    def bad(value):
        raise TimingError('Non-finite JSON value: ' + value)
    return json.loads(Path(path).read_text(encoding='utf-8-sig'), parse_constant=bad)

def finite(value, label):
    need(type(value) in (float, int), label + ': explicit numeric value required')
    need(math.isfinite(value), label + ': non-finite')
    return float(value)

def text(value, label):
    need(isinstance(value, str) and len(value.strip()) >= 3 and
         value.strip().upper() not in {'PENDING', 'UNKNOWN', 'TBD', 'TODO'}, label + ': missing review text')

def project_file(root, name):
    need(isinstance(name, str) and name and not Path(name).is_absolute(), 'Evidence must use a project-relative path')
    path = (root / name).resolve()
    need(path.is_relative_to(root) and path.is_file(), 'Missing/escaping evidence: ' + name)
    return path

def check_evidence(root, contract):
    evidence = contract.get('evidence', [])
    need(isinstance(evidence, list), 'evidence must be an array')
    hashes = {}
    for item in evidence:
        name = item.get('id')
        need(isinstance(name, str) and name and name not in hashes, 'Missing/duplicate evidence ID')
        path = project_file(root, item.get('path'))
        need(re.fullmatch('[0-9a-f]{64}', item.get('sha256', '')) is not None, 'Invalid evidence SHA: ' + name)
        need(sha(path) == item['sha256'], 'Stale/tampered evidence: ' + name)
        text(item.get('kind'), 'Evidence kind ' + name)
        hashes[name] = dict(item)
    return hashes

def refs(value, evidence, label):
    need(isinstance(value, list) and value and len(value) == len(set(value)) and
         all(v in evidence for v in value), label + ': missing/duplicate/unbound evidence references')

def review(section, evidence, label):
    approval = section.get('review', {})
    for key in ('reviewer', 'date', 'scope'):
        text(approval.get(key), label + ' review ' + key)
    refs(approval.get('evidence_refs'), evidence, label + ' signed review')

def source_values(section, evidence):
    review(section, evidence, 'source')
    for key in ('reference_plane', 'operating_conditions', 'relative_uncertainty_accounting', 'path_coverage'):
        text(section.get(key), 'source ' + key)
    need(section.get('no_double_counting_reviewed') is True, 'External path/jitter double counting has not been reviewed')
    firmware = section.get('firmware', {})
    for key in ('bin_evidence', 'cfg_evidence'):
        refs([firmware.get(key)], evidence, 'Actual firmware ' + key)
    for key in ('version', 'cli_version', 'output_mode', 'lane_mapping_and_polarity', 'frame_clock_relation'):
        text(firmware.get(key), 'Actual firmware ' + key)
    need(firmware.get('confirmed_running_on_this_board') is True, 'Firmware is not confirmed as actually running')
    need(firmware.get('lanes') == 2 and firmware.get('rate_mbps_per_lane') == 600 and
         firmware.get('bitclk_mhz') == 300 and firmware.get('word_bits') == 16, 'Firmware mode differs from current PHY')
    scope = section.get('idelay_scope', {})
    need(scope.get('mode') == 'RESET_CALIBRATED_INITIAL_ONLY', 'Only the reset-calibrated initial IDELAY point is supported by this STA qualification')
    need(finite(scope.get('sta_delay_value_ps'), 'IDELAY STA point') == 0, 'IDELAY STA point differs from current primitive DELAY_VALUE')
    text(scope.get('excluded_software_taps'), 'Explicit exclusion of runtime IDELAY tap changes')
    refs(scope.get('evidence_refs'), evidence, 'IDELAY scope')
    lanes = section.get('lanes', {})
    need(set(lanes) == set(PORTS), 'Exactly DATA0, DATA1 and FRCLK positive input lanes are required')
    result = {}
    for port in PORTS:
        need(set(lanes[port]) == set(EDGES), port + ': both clock reference edges required')
        result[port] = {}
        for edge in EDGES:
            row = lanes[port][edge]
            text(row.get('reference_edge_relation'), port + ' ' + edge + ' relation')
            for category in ('source', 'path', 'margin'):
                refs(row.get('evidence_refs', {}).get(category), evidence, port + ' ' + edge + ' ' + category)
            values = {key: finite(row.get(key), port + ' ' + edge + ' ' + key) for key in
                      ('source_min_ns', 'source_max_ns', 'data_path_min_ns', 'data_path_max_ns',
                       'clock_path_min_ns', 'clock_path_max_ns', 'early_margin_ns', 'late_margin_ns')}
            for prefix in ('source', 'data_path', 'clock_path'):
                need(values[prefix+'_min_ns'] <= values[prefix+'_max_ns'], port + ' ' + edge + ' reversed ' + prefix + ' range')
            for key in ('data_path_min_ns', 'clock_path_min_ns', 'early_margin_ns', 'late_margin_ns'):
                need(values[key] >= 0, port + ' ' + edge + ' negative path/margin')
            exact = {key: Decimal(str(value)) for key, value in values.items()}
            low = exact['source_min_ns'] + exact['data_path_min_ns'] - exact['clock_path_max_ns'] - exact['early_margin_ns']
            high = exact['source_max_ns'] + exact['data_path_max_ns'] - exact['clock_path_min_ns'] + exact['late_margin_ns']
            need(low.is_finite() and high.is_finite() and low <= high, 'Invalid computed input delay')
            # Native write_xdc exposes a 1 ps grid. Widen the interval explicitly;
            # never weaken matching tolerance to conceal a rounded-in bound.
            result[port][edge] = {'min': float(low.quantize(Decimal('.001'), rounding=ROUND_FLOOR)),
                                 'max': float(high.quantize(Decimal('.001'), rounding=ROUND_CEILING)),
                                 'raw_min': float(low), 'raw_max': float(high)}
    return result

def board_values(section, evidence):
    review(section, evidence, 'board')
    for key in ('f20_core_revision', 'f20_carrier_revision', 'adapter_revision', 'radar_revision', 'assembly_identity',
                'power_ground_polarity_review', 'power_off_compatibility_review'):
        text(section.get(key), 'board ' + key)
    need(section.get('actual_board_confirmed') is True, 'Actual assembled board has not been confirmed')
    need(finite(section.get('bank65_vcco_v'), 'Bank65 VCCO') == 1.8, 'Current HP LVDS internal termination requires confirmed 1.8V Bank65')
    need(section.get('bank65_confirmation') in ('MEASURED_AND_REVIEWED', 'JUMPER_CONFIRMED_BY_USER'), 'Bank65 confirmation method missing')
    refs(section.get('bank65_evidence_refs'), evidence, 'Actual Bank65 VCCO')
    for bank, voltage in (('bank64', 1.2), ('bank44', 3.3)):
        need(finite(section.get(bank+'_vcco_v'), bank+' VCCO') == voltage, bank+' actual VCCO differs from current standards')
        refs(section.get(bank+'_evidence_refs'), evidence, bank+' electrical conditions')
    text(section.get('bank64_ac_bias_external_termination'), 'Bank64 reference AC coupling/bias/termination')
    termination = section.get('bank65_termination', {})
    need(termination.get('receiver_internal_ohms') == 100, 'Expected internal 100 ohm receiver termination')
    segments = termination.get('segments', {})
    need(set(segments) == {'radar_board', 'cable_connectors', 'adapter', 'f20'}, 'Complete termination chain review required')
    for name, segment in segments.items():
        refs(segment.get('evidence_refs'), evidence, name + ' termination')
        need(segment.get('external_diff_ohms') == [], 'External termination conflicts with unchanged internal 100 ohm scheme: ' + name)
        text(segment.get('review'), name + ' full signal path review')

def assess_contract(root, contract_path=None):
    root = Path(root).resolve()
    path = Path(contract_path) if contract_path else root / CONTRACT
    value = read(path)
    need(value.get('schema') == 1, 'Unsupported R07 contract schema')
    evidence = check_evidence(root, value)
    result = {'SOURCE_TIMING': 'PENDING', 'BOARD_ELECTRICAL': 'PENDING', 'eligible': False,
              'issues': [], 'contract_sha256': sha(path), 'evidence_hashes': evidence, 'effective_delays': {},
              'delay_quantization': 'Outward to 0.001 ns: min=floor(raw_min), max=ceil(raw_max); raw values retained.'}
    for key, state, validator in (('source_timing', 'VERIFIED_WITH_SCOPE', source_values),
                                  ('board_electrical', 'CONFIRMED_FOR_THIS_BOARD', board_values)):
        section = value.get(key, {})
        status = section.get('status')
        need(status in ('PENDING', state), 'Unrecognized ' + key + ' status')
        if status == 'PENDING':
            pending = section.get('pending_reasons')
            need(isinstance(pending, list) and pending and all(isinstance(p, str) and p.strip() for p in pending), key + ' pending reasons required')
            result['issues'].extend(key + ': ' + item for item in pending)
        else:
            checked = validator(section, evidence)
            result[key.upper()] = state
            if key == 'source_timing':
                result['effective_delays'] = checked
    result['eligible'] = result['SOURCE_TIMING'] == 'VERIFIED_WITH_SCOPE' and result['BOARD_ELECTRICAL'] == 'CONFIRMED_FOR_THIS_BOARD'
    return result

def render_xdc(result):
    need(result['SOURCE_TIMING'] == 'VERIFIED_WITH_SCOPE', 'No numeric XDC may be generated from pending source evidence')
    lines = ['# R07 reviewed source-synchronous input delays, ns; generated by source_timing.py.',
             '# Contract SHA256: ' + result['contract_sha256'],
             '# Reference: RADAR_BCLK300 at FPGA BITCLK P input. No negative-port duplicate.',
             '# Scope: reset-calibrated IDELAY initial DELAY_VALUE=0 ps only; runtime taps excluded.']
    for name, item in sorted(result['evidence_hashes'].items()):
        lines.append('# Evidence ' + name + ': ' + item['path'] + ' SHA256=' + item['sha256'])
    for port in PORTS:
        for edge in EDGES:
            row = result['effective_delays'][port][edge]
            lines.append(f'# {port} {edge}: raw [{row["raw_min"]:.12g}, {row["raw_max"]:.12g}] ns; outward rounded to 1 ps.')
            flags = ' -clock_fall -add_delay' if edge == 'fall' else ''
            for bound in ('min', 'max'):
                number = format(result['effective_delays'][port][edge][bound], '.12g')
                lines.append(f'set_input_delay -clock [get_clocks RADAR_BCLK300]{flags} -{bound} {number} [get_ports {port}]')
    return '\n'.join(lines) + '\n'

def effective_input_delays(content):
    """Parse the native write_xdc declarations, not the requested input file."""
    found = {}
    for line in content.replace('\\\n', ' ').splitlines():
        line = line.strip()
        if not line.startswith('set_input_delay ') or not any(p in line for p in PORTS):
            continue
        clock = re.search(r'-clock\s+\[get_clocks\s+\{?(\w+)\}?\]', line)
        port = re.search(r'\[get_ports\s+\{?(\w+)\}?\]', line)
        need(clock and clock[1] == 'RADAR_BCLK300' and port and port[1] in PORTS, 'Unknown/aggregate native input delay declaration: ' + line)
        need(not re.search(r'\s-(?:rise|fall|reference_pin|source_latency_included|network_latency_included)(?:\s|$)', line), 'Unsupported native input delay options')
        bounds = [name for name in ('min', 'max') if re.search(r'\s-' + name + r'(?:\s|$)', line)]
        need(len(bounds) == 1, 'Native input delay must explicitly select min or max')
        edge = 'fall' if '-clock_fall' in line else 'rise'
        need(edge != 'fall' or '-add_delay' in line, 'Native falling reference edge must preserve rising constraint')
        tail = re.sub(r'\[[^\]]*\]', '', line)
        numbers = re.findall(r'(?<!\S)[+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?(?!\S)', tail)
        need(len(numbers) == 1, 'Native delay numeric operand is ambiguous')
        key = (port[1], edge, bounds[0])
        need(key not in found, 'Duplicate native input delay: ' + str(key))
        found[key] = finite(float(numbers[0]), 'Native input delay')
    return found

def qualify_r07(root, report_dir):
    root, reports = Path(root).resolve(), Path(report_dir).resolve()
    result = assess_contract(root)
    native = read(reports / 'r07_native.json')
    need(native.get('schema') == 1 and native.get('tool_version') == '2025.1' and native.get('stage') == 'route', 'Native R07 route evidence missing/wrong tool')
    result['native_status'] = 'PENDING_SOURCE_CONSTRAINTS'
    # Electrical declaration remains independently checkable with physical facts pending.
    ports = native.get('ports', {})
    balls = dict(zip(('radar_bitclk_p', 'radar_bitclk_n', 'radar_frclk_p', 'radar_frclk_n', 'radar_data0_p', 'radar_data0_n', 'radar_data1_p', 'radar_data1_n'), ('L7','L6','L3','L2','K8','K7','J6','H6')))
    for port, ball in balls.items():
        props = ports.get(port, {})
        need(props.get('PACKAGE_PIN') == ball and props.get('IOSTANDARD') == 'LVDS', 'Final Bank65 pin/standard mismatch: ' + port)
        need(props.get('DIFF_TERM_ADV') == 'TERM_100', 'Final Bank65 PORT termination missing: ' + port)
    other = {'phy_ref_clk_100m_clk_p':('AE5','DIFF_SSTL12'), 'phy_ref_clk_100m_clk_n':('AF5','DIFF_SSTL12'),
             'radar_uart_tx':('AG10','LVCMOS33'), 'radar_uart_rx':('AH10','LVCMOS33'),
             'radar_nerr_n':('AE12','LVCMOS33'), 'radar_3v3_present':('AF12','LVCMOS33'),
             'radar_sync':('AE10','LVCMOS33'), 'radar_nrst_n':('AF10','LVCMOS33')}
    for port, (ball, standard) in other.items():
        props = ports.get(port, {})
        need(props.get('PACKAGE_PIN') == ball and props.get('IOSTANDARD') == standard, 'Final Bank64/44 pin/standard mismatch: ' + port)
        if standard == 'DIFF_SSTL12':
            need(str(props.get('DIFF_TERM', '')).lower() in ('','0','false') and props.get('DIFF_TERM_ADV','') in ('','TERM_NONE'), 'Unexpected reference internal termination')
    ibufs = native.get('ibufs', {})
    need(set(ibufs) == {'u_bclk_ibuf','u_fr_ibuf','u_d0_ibuf','u_d1_ibuf'}, 'Missing final Bank65 IBUFDS objects')
    for name, props in ibufs.items():
        need(props.get('REF_NAME') == 'IBUFDS', 'Wrong final input primitive: ' + name)
        need(props.get('IOSTANDARD') == 'LVDS', 'Final input primitive standard conflict: ' + name)
        stem = {'u_bclk_ibuf':'radar_bitclk', 'u_fr_ibuf':'radar_frclk', 'u_d0_ibuf':'radar_data0', 'u_d1_ibuf':'radar_data1'}[name]
        need(props.get('I_PORT') == stem+'_p' and props.get('IB_PORT') == stem+'_n', 'Final IBUFDS positive/negative connectivity mismatch: ' + name)
    # UG912 DIFF_TERM_ADV applies to PORT. In this mapped UltraScale+ design,
    # the legacy IBUFDS DIFF_TERM default reads 0 while PORT is TERM_100;
    # do not confuse a legacy primitive default with effective port termination.
    result['termination_authority'] = 'Implemented PORT DIFF_TERM_ADV=TERM_100; IBUFDS legacy DIFF_TERM default is recorded, not used to override PORT.'
    result['design_io_status'] = 'PASS'
    clocks = native.get('clocks', {})
    native_bclk_period = float(clocks.get('RADAR_BCLK300', {}).get('PERIOD', 'nan'))
    need(math.isfinite(native_bclk_period) and abs(native_bclk_period-3.333333) <= .000501, 'Native BITCLK period differs from 300 MHz supported point')
    for name, divide in (('RADAR_BCLK300',1), ('RADAR_SER300',1), ('RADAR_RX75',4)):
        props = clocks.get(name, {})
        period = float(props.get('PERIOD', 'nan'))
        need(math.isfinite(period) and abs(period-native_bclk_period*divide) <= .000501, 'Native clock period mismatch: ' + name)
        waveform = [float(v) for v in str(props.get('WAVEFORM', '')).split()]
        need(len(waveform) == 2 and all(math.isfinite(v) for v in waveform) and abs(waveform[0]) <= .000501 and abs(waveform[1]-period/2) <= .000501, 'Native clock waveform mismatch: ' + name)
        if name != 'RADAR_BCLK300':
            need(props.get('MASTER_CLOCK') == 'RADAR_BCLK300' and str(props.get('IS_GENERATED')).lower() in ('1','true'), 'Native generated clock relationship mismatch: ' + name)
    actual = effective_input_delays((reports / 'r07_effective_timing.xdc').read_text(encoding='utf-8'))
    if result['SOURCE_TIMING'] != 'VERIFIED_WITH_SCOPE':
        need(not native.get('active_source_xdc') and not native.get('active_template_xdc'), 'Unreviewed source timing/template unexpectedly active')
        need(not actual, 'Unreviewed high-speed input delay active under another filename')
        result['eligible'] = False
        return result
    need(native.get('active_source_xdc') is True and not native.get('active_template_xdc'), 'Final source XDC is not exclusively active')
    xdc = root / XDC
    need(xdc.is_file() and xdc.read_text(encoding='utf-8') == render_xdc(result), 'Active source XDC differs from reviewed contract')
    expected = {(p,e,b): result['effective_delays'][p][e][b] for p in PORTS for e in EDGES for b in ('min','max')}
    need(set(actual) == set(expected) and all(abs(actual[k]-expected[k]) <= 0.000001 for k in expected), 'Native dual-edge constraint values/coverage differ from outward-rounded contract')
    rows = native.get('paths', [])
    need(len(rows) == 12 and {(r.get('port'),r.get('edge'),r.get('bound')) for r in rows} == set(expected), 'Native timing path coverage is incomplete')
    for row in rows:
        need(row.get('path_count', 0) > 0 and row.get('ignored_path_count') == 0, 'Missing/ignored high-speed input timing path')
        props = row.get('worst', {})
        need(props.get('STARTPOINT_PIN') == row['port'] and props.get('ENDPOINT_PIN') == BASE+SERDES[row['port']]+'/D', 'Native path start/end differs from physical lane')
        need(props.get('STARTPOINT_CLOCK') == 'RADAR_BCLK300' and props.get('ENDPOINT_CLOCK') == 'RADAR_SER300', 'Input path uses unrelated source/capture clock')
        need(props.get('DELAY_TYPE') == row['bound'] and props.get('EXCEPTION') == '', 'Unexpected path analysis type or timing exception')
        launch = float(props.get('STARTPOINT_CLOCK_EDGE', 'nan'))
        period = float(clocks['RADAR_BCLK300']['PERIOD'])
        launch_waveform = [float(v) for v in clocks['RADAR_BCLK300']['WAVEFORM'].split()]
        phase = (launch - launch_waveform[0 if row['edge'] == 'rise' else 1]) % period
        need(math.isfinite(launch) and min(phase, period-phase) <= .000501, 'Wrong launch clock edge')
        value = float(props.get('INPUT_DELAY', 'nan'))
        need(math.isfinite(value) and abs(value-expected[(row['port'],row['edge'],row['bound'])]) <= .000001, 'Native path input delay differs from reviewed value')
        need(float(props.get('SLACK', 'nan')) >= 0 and math.isfinite(float(props.get('SLACK', 'nan'))), 'Input setup/hold slack failed or unavailable')
        report = (reports / f'r07_{row["port"]}_{row["edge"]}_{row["bound"]}.rpt').read_text(encoding='utf-8')
        slack = re.search(r'Slack\s*\((MET|VIOLATED)\)\s*:\s*([-+0-9.eE]+)ns', report)
        need(slack is not None and slack[1] == 'MET' and math.isfinite(float(slack[2])) and float(slack[2]) >= 0, 'Detailed input timing report fails setup/hold')
    cells = native.get('delay_cells', {})
    need(len(cells) == 3, 'Exactly three delay primitives required')
    for port in PORTS:
        props = cells.get(BASE+f'g_lane[{LANES[port]}].u_delay', {})
        need(props.get('DELAY_FORMAT') == 'TIME' and props.get('DELAY_TYPE') == 'VAR_LOAD' and str(props.get('DELAY_VALUE')) in ('0','0.000'), 'STA IDELAY point differs from reviewed initial point')
    result['native_status'] = 'PASS'
    return result

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('inspect', 'generate-xdc', 'qualify'))
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--reports', type=Path)
    args = parser.parse_args()
    result = qualify_r07(args.root, args.reports) if args.action == 'qualify' else assess_contract(args.root)
    if args.action == 'generate-xdc':
        need(args.output is not None, 'Explicit --output required')
        content = render_xdc(result)
        with args.output.open('x', encoding='utf-8', newline='\n') as f:
            f.write(content)
    print(json.dumps(result, indent=2, ensure_ascii=False, allow_nan=False))

if __name__ == '__main__':
    try:
        main()
    except (TimingError, KeyError, TypeError, ValueError, OSError) as error:
        print('R07 ERROR: ' + str(error), file=sys.stderr)
        raise SystemExit(2)
