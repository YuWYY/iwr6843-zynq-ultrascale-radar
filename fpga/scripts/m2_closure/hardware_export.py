"""Verify fresh native hardware exports against the current sealed routed build."""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import sys
from types import SimpleNamespace
import xml.etree.ElementTree as ET
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from build_identity import GateError, require, sha, read_json, write_new, under, run_log_records
from release_gate import qualify_current, drc_critical


def qualification_args(root, build, native, simulation):
    return SimpleNamespace(root=root, reports=build/'reports_route',
        route_receipt=build/'route_identity.json', stage_status=build/'route_STATUS.json',
        policy=root/'scripts/m2_closure/release_policy.json', native=native,
        manifest=build/'build_manifest.json', simulation=simulation,
        synth_receipt=build/'synth_identity.json',
        synth_dcp=root/'Radar_XCZU2CG_M2.runs/synth_1/radar_m2_top.dcp')


def eligible(result, mode):
    require(mode in {'dev-hw', 'candidate'}, 'Unknown hardware export mode')
    require(result.get('internal_status') == 'PASS' and result.get('SIMULATION') == 'PASS'
            and result.get('NATIVE_ROUTE_EXECUTION') == 'PASS', 'Current internal route/simulation gate is not PASS')
    if mode == 'candidate':
        require(result.get('BITSTREAM_ELIGIBLE') == 'YES' and
                result.get('SOURCE_TIMING') == 'VERIFIED_WITH_SCOPE' and
                result.get('BOARD_ELECTRICAL') == 'CONFIRMED_FOR_THIS_BOARD',
                'R07 actual source timing/board evidence is incomplete; bitgen is prohibited')
    # BOARD_TEST deliberately does not gate a first download candidate.


def hwh_contract(data):
    require(b'<!DOCTYPE' not in data.upper(), 'Unexpected DTD in hardware handoff')
    tree = ET.fromstring(data)
    modules = list(tree.iter('MODULE'))
    dma = [m for m in modules if m.get('MODTYPE') == 'axi_dma']
    gpio = [m for m in modules if m.get('MODTYPE') == 'axi_gpio']
    require(len(dma) == len(gpio) == 1, 'Export must contain exactly one DMA and one GPIO')
    require(dma[0].get('INSTANCE') == 'axi_dma_capture' and gpio[0].get('INSTANCE') == 'axi_gpio_ctrl', 'Wrong hardware instance identity')
    def parameters(module, expected):
        rows = module.findall('./PARAMETERS/PARAMETER')
        values = {p.get('NAME'):p.get('VALUE') for p in rows}
        require(len(values) == len(rows), 'Duplicate HWH parameter')
        for key, value in expected.items():
            require(key in values and int(values[key], 0) == value, f'Wrong exported {key}: {values.get(key)}')
        return {key: values[key] for key in expected}
    d = parameters(dma[0], {'C_INCLUDE_SG':1, 'C_INCLUDE_MM2S':0, 'C_INCLUDE_S2MM':1,
        'C_S_AXIS_S2MM_TDATA_WIDTH':64, 'C_M_AXI_S2MM_DATA_WIDTH':64})
    g = parameters(gpio[0], {'C_IS_DUAL':1, 'C_GPIO_WIDTH':32, 'C_GPIO2_WIDTH':32})
    ranges = list(tree.iter('MEMRANGE'))
    selected = []
    for instance, interface, base, high in (
        ('axi_dma_capture', 'S_AXI_LITE', 0xA0000000, 0xA000FFFF),
        ('axi_gpio_ctrl', 'S_AXI', 0xA0010000, 0xA001FFFF)):
        rows = [r for r in ranges if r.get('INSTANCE') == instance and r.get('SLAVEBUSINTERFACE') == interface]
        require(len(rows) == 1 and int(rows[0].get('BASEVALUE'), 0) == base and int(rows[0].get('HIGHVALUE'), 0) == high, f'Wrong CSR address in XSA: {instance}')
        selected.append(dict(rows[0].attrib))
    for interface in ('M_AXI_SG', 'M_AXI_S2MM'):
        rows = [r for r in dma[0].iter('MEMRANGE') if r.get('MASTERBUSINTERFACE') == interface]
        require(len(rows) == 1 and rows[0].get('SLAVEBUSINTERFACE') == 'S_AXI_HP0_FPD'
                and int(rows[0].get('BASEVALUE'), 0) == 0 and int(rows[0].get('HIGHVALUE'), 0) == 0x7FFFFFFF,
                f'Wrong HP0 DDR mapping in XSA: {interface}')
        selected.append(dict(rows[0].attrib))
    return {'DMA':d, 'GPIO':g, 'addresses':selected}


def inspect_xsa(xsa, mode, actual_hwh, bit=None):
    require(xsa.is_file() and xsa.stat().st_size > 0, 'Missing/empty native XSA')
    expected = 'radar_system_m2.xsa' if mode == 'candidate' else 'radar_system_m2_dev_hw.xsa'
    require(xsa.name == expected, 'Ambiguous exported XSA filename')
    with zipfile.ZipFile(xsa) as z:
        members = z.infolist()
        names = [i.filename for i in members]
        require(names and len(names) == len(set(names)), 'Empty/duplicate XSA members')
        for item in members:
            p = PurePosixPath(item.filename.replace('\\', '/'))
            require(not p.is_absolute() and '..' not in p.parts and ':' not in str(p) and not item.flag_bits & 1, 'Unsafe/encrypted XSA member')
        require(z.testzip() is None, 'XSA CRC verification failed')
        bits = [n for n in names if n.lower().endswith('.bit')]
        require(not any(n.lower().endswith(('.bin', '.mcs', '.pdi')) for n in names), 'Binary/flash image unexpectedly included')
        if mode == 'candidate':
            require(bit is not None and bit.is_file() and bit.stat().st_size > 0, 'Missing standalone candidate bit')
            require(len(bits) == 1 and hashlib.sha256(z.read(bits[0])).hexdigest() == sha(bit), 'Embedded XSA bit differs from standalone current bit')
        else:
            require(not bits and bit is None, 'Development hardware description must not include a PL image')
        hwh = [n for n in names if PurePosixPath(n).name == 'radar_system.hwh']
        require(len(hwh) == 1, 'Missing/ambiguous top hardware handoff')
        data = z.read(hwh[0])
        require(actual_hwh.is_file() and hashlib.sha256(data).hexdigest() == sha(actual_hwh), 'XSA HWH differs from current native generated hardware handoff')
        hardware = hwh_contract(data)
        hashes = {n:hashlib.sha256(z.read(n)).hexdigest() for n in sorted(names) if not n.endswith('/')}
    return {'path':xsa.name, 'sha256':sha(xsa), 'size':xsa.stat().st_size,
        'embedded_bit': bits[0] if bits else None, 'members': hashes, 'hardware':hardware}


def native_session_log(content, mode, build_id, process_exit_code):
    """Accept only a closed native Vivado session for this exact export attempt."""
    require(process_exit_code == 0, 'Native export process did not exit successfully')
    require(re.search(r'^# Vivado v2025\.1 \(64-bit\)\s*$', content, re.M), 'Native Vivado 2025.1 log header missing')
    commands = re.findall(r'^# Command line\s*:\s*(.*)$', content, re.M)
    require(len(commands) == 1 and re.search(r'-tclargs\s+' + re.escape(mode) + r'\s+' + re.escape(build_id) + r'(?:\s|$)', commands[0]), 'Native command line belongs to another export mode/build')
    marker = 'M2_EXPORT_NATIVE_COMPLETE ' + mode + ' ' + build_id
    require(len(re.findall(r'^' + re.escape(marker) + r'\s*$', content, re.M)) == 1, 'Native completion marker missing or duplicated')
    lines = content.rstrip().splitlines()
    require(content.endswith('\n') and lines and re.match(r'^INFO: \[Common 17-206\] Exiting Vivado at .+\.\.\.$', lines[-1]), 'Native log is incomplete or lacks its final exit record')
    require(content.rfind(lines[-1]) > content.index(marker), 'Native completion/exit ordering invalid')
    require(not run_log_records(content, 'hardware_export'), 'Native export log contains ERROR/Critical')
    return {'tool_version':'2025.1', 'mode':mode, 'build_id':build_id, 'process_exit_code':process_exit_code,
            'native_completion_marker':marker, 'exit_record':lines[-1]}


def seal(args, result, export):
    preflight = read_json(export/'preflight.json')
    require(preflight.get('mode') == args.mode and preflight.get('qualification') == result, 'Export preflight/current qualification changed')
    stage_path = args.build/(args.mode+'_STATUS.json')
    stage = read_json(stage_path)
    require(stage.get('stage') == args.mode and stage.get('build_id') == result['build_id'] and
            stage.get('state') == 'NATIVE_COMPLETE' and stage.get('DOWNLOAD_CANDIDATE') == 'NO',
            'Only a current native-complete, not already published export can be sealed')
    require(args.native_log is not None and args.native_log.resolve().parent == args.build and args.native_log.suffix == '.log',
            'Closed native session log must belong to this build output directory')
    log_bytes = args.native_log.read_bytes()
    log_text = log_bytes.decode('utf-8-sig', errors='strict')
    log_audit = native_session_log(log_text, args.mode, result['build_id'], args.process_exit_code)
    native = read_json(export/'native_export_receipt.json')
    require(native.get('mode') == args.mode and native.get('tool_version') == '2025.1'
            and native.get('top') == 'radar_m2_top' and native.get('run_status') == 'route_design Complete!'
            and native.get('completed') is True, 'Native export did not complete on the current routed design')
    require(native.get('include_bit') is (args.mode == 'candidate') and native.get('include_bin') is False, 'Wrong native XSA export options')
    hwh = args.root/'Radar_XCZU2CG_M2.gen/sources_1/bd/radar_system/hw_handoff/radar_system.hwh'
    bit = export/'radar_system_m2.bit' if args.mode == 'candidate' else None
    if args.mode == 'candidate':
        actual_bit = args.root/'Radar_XCZU2CG_M2.runs/impl_1/radar_m2_top.bit'
        require(native.get('bitgen_command') == 'write_bitstream' and native.get('bitgen_completed') is True, 'Actual native bitgen was not completed')
        require(actual_bit.is_file() and sha(actual_bit) == sha(bit), 'Standalone bit differs from current implementation bit')
        drc = (export/'bitgen_drc.rpt').read_text(encoding='utf-8-sig')
        require(not drc_critical(drc), 'Bitgen DRC contains Critical/Error')
        require(any('write_bitstream completed successfully' in line and not line.lstrip().startswith('#')
                    for line in log_text.splitlines()), 'Native bitgen success marker missing')
    xsa = export/('radar_system_m2.xsa' if args.mode == 'candidate' else 'radar_system_m2_dev_hw.xsa')
    audit = inspect_xsa(xsa, args.mode, hwh, bit)
    # Complete native output, not a synthetic success-only command log.
    with (export/'native_session.log').open('xb') as f:
        f.write(log_bytes)
    require(sha(export/'native_session.log') == sha(args.native_log), 'Native log changed during post-process sealing')
    write_new(export/'native_completion_status.json', stage)
    log_audit.update({'path':args.native_log.relative_to(args.root).as_posix(), 'sha256':sha(args.native_log)})
    records = {p.name:sha(p) for p in sorted(export.iterdir()) if p.is_file()}
    require(not (export/'export_manifest.json').exists(), 'Existing export cannot be resealed')
    value = {'schema':1, 'mode':args.mode, 'status':'PASS', 'build_id':result['build_id'],
        'SOURCE_TIMING':result['SOURCE_TIMING'], 'BOARD_ELECTRICAL':result['BOARD_ELECTRICAL'],
        'DOWNLOAD_CANDIDATE':'YES' if args.mode == 'candidate' else 'NO', 'BOARD_TEST':'NOT_RUN',
        'purpose':'SCOPED_DOWNLOAD_CANDIDATE' if args.mode == 'candidate' else 'VITIS_COMPILE_ONLY_NO_PL_IMAGE',
        'build_manifest_sha256':sha(args.build/'build_manifest.json'),
        'synth_identity_sha256':sha(args.build/'synth_identity.json'),
        'route_identity_sha256':sha(args.build/'route_identity.json'), 'qualification':result,
        'native_inventory_sha256':sha(args.native), 'native_session':log_audit, 'generated_hwh_sha256':sha(hwh),
        'bit_sha256':sha(bit) if bit else None, 'xsa':audit, 'files':records}
    write_new(export/'export_manifest.json', value)
    stage.update({'state':'PASS', 'DOWNLOAD_CANDIDATE':value['DOWNLOAD_CANDIDATE'],
                  'message':'Closed native log and current hardware artifacts qualified; see export_manifest.json'})
    stage_path.write_text(json.dumps(stage, indent=2)+'\n', encoding='utf-8')
    return value


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('action', choices=['preflight', 'seal'])
    p.add_argument('--mode', choices=['dev-hw','candidate'], required=True)
    p.add_argument('--root', type=Path, required=True)
    p.add_argument('--build', type=Path, required=True)
    p.add_argument('--native', type=Path, required=True)
    p.add_argument('--simulation', type=Path, required=True)
    p.add_argument('--native-log', type=Path, help='seal only: complete native Vivado -log after process exit')
    p.add_argument('--process-exit-code', type=int, help='seal only: actual native Vivado process exit code')
    args = p.parse_args()
    try:
        args.root = args.root.resolve(); args.build = args.build.resolve()
        require(under(args.build, args.root/'outputs'), 'Export build directory must be a current project output')
        export = args.build/args.mode
        result = qualify_current(qualification_args(args.root, args.build, args.native, args.simulation))
        eligible(result, args.mode)
        if args.action == 'preflight':
            require(not any(export.glob('*.xsa')) and not any(export.glob('*.bit')), 'Export output already contains artifacts')
            write_new(export/'preflight.json', {'schema':1,'mode':args.mode,'qualification':result})
            output = {'mode':args.mode, 'preflight':'PASS', 'build_id':result['build_id']}
        else:
            output = seal(args, result, export)
        print(json.dumps(output, indent=2))
        return 0
    except (GateError, OSError, ValueError, KeyError, TypeError, ET.ParseError, zipfile.BadZipFile) as error:
        # Preserve the failed attempt; never leave a post-process failure as PASS
        # or downgrade a previously completed export on a second invocation.
        if args.action == 'seal' and 'export' in locals() and under(args.build, args.root/'outputs'):
            status_path = args.build/(args.mode+'_STATUS.json')
            if status_path.is_file():
                failed = read_json(status_path)
                if failed.get('stage') == args.mode and failed.get('state') == 'NATIVE_COMPLETE':
                    failed.update({'state':'FAIL', 'DOWNLOAD_CANDIDATE':'NO', 'message':'Post-process qualification failed: '+str(error)})
                    status_path.write_text(json.dumps(failed, indent=2)+'\n', encoding='utf-8')
        print(f'HARDWARE_EXPORT_FAIL: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
