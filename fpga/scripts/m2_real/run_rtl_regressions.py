"""Run current-production RTL in Vivado 2025.1 XSim without opening a project."""
import argparse
import hashlib
import json
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
VIVADO = pathlib.Path('C:/Xilinx/2025.1/Vivado')

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--label', required=True)
    ap.add_argument('--tops', nargs='+', default=['tb_delay_transaction', 'tb_early_clock_loss', 'tb_control'])
    ap.add_argument('--expect-fail', action='store_true')
    ap.add_argument('--phase', type=float, choices=[0, 0.25, 0.75, 1.25, 2.25], default=0.75)
    args = ap.parse_args()
    out = ROOT / 'outputs/m2_real_20261012_171114/rtl_regressions' / args.label
    out.mkdir(parents=True, exist_ok=False)
    result = {'label': args.label, 'phase_ns': args.phase, 'project_opened': False, 'cases': []}
    sources = sorted(str(p) for group in ['common', 'control', 'phy', 'capture'] for p in (ROOT/'rtl'/group).glob('*.v'))
    sources += [str(VIVADO/'data/verilog/src/glbl.v')]
    result['status'] = 'RUNNING'
    result['input_sha256'] = {str(pathlib.Path(p).relative_to(ROOT)): hashlib.sha256(pathlib.Path(p).read_bytes()).hexdigest()
                              for p in sources[:-1]+[str(ROOT/'sim'/f'{top}.sv') for top in args.tops]}
    def save():
        (out/'summary.json').write_text(json.dumps(result, indent=2)+'\n', encoding='utf-8')
    def fail(message):
        result.update(status='FAIL', success=False, error=message);save()
        raise RuntimeError(message)
    save()
    def tool(name, argv, logfile):
        command = [str(VIVADO/'bin'/f'{name}.bat'), *argv]
        with (out/logfile).open('w', encoding='utf-8') as log:
            log.write('COMMAND: '+subprocess.list2cmdline(command)+'\n'); log.flush()
            try:
                completed = subprocess.run(command, cwd=out, stdout=log, stderr=subprocess.STDOUT, timeout=240)
            except (OSError, subprocess.TimeoutExpired) as exc:
                fail(str(exc))
        content = (out/logfile).read_text(encoding='utf-8', errors='replace')
        return completed.returncode, content
    # Production .v files are Verilog, not SystemVerilog (e.g. matches is a
    # legal Verilog identifier). Preserve the project's declared file types.
    code, _ = tool('xvlog', sources, 'xvlog_verilog_console.log')
    if code: fail(f'xvlog failed: {out}')
    code, _ = tool('xvlog', ['--sv', *[str(ROOT/'sim'/f'{top}.sv') for top in args.tops]], 'xvlog_testbench_console.log')
    if code: fail(f'xvlog failed: {out}')
    for top in args.tops:
        snapshot = top+'_snapshot'
        code, _ = tool('xelab', ['--debug', 'typical', '--relax', '-L', 'unisims_ver', '-L', 'unimacro_ver', '-L', 'secureip', top, 'glbl', '-s', snapshot], f'{top}_xelab.log')
        if code: fail(f'xelab failed for {top}: {out}')
        code, log = tool('xsim', [snapshot, '-runall', '-testplusarg', f'PHASE{round(args.phase*100):03d}', '-log', f'{top}_native.log'], f'{top}_xsim.log')
        fatal = 'Fatal:' in log or 'FATAL' in log or 'ERROR:' in log
        passed = code == 0 and f'PASS {top}' in log and not fatal
        reproduced = not passed and 'REGRESSION_ASSERT' in log
        result['cases'].append({'top': top, 'returncode': code, 'pass': passed, 'expected_failure_reproduced': reproduced if args.expect_fail else False})
        print(f'{top}: pass={passed}, reproduced={reproduced}, log={out/top}', flush=True)
    success = all(c['expected_failure_reproduced'] if args.expect_fail else c['pass'] for c in result['cases'])
    result['success'] = success
    result['status'] = 'EXPECTED_FAILURE_REPRODUCED' if success and args.expect_fail else ('PASS' if success else 'FAIL')
    save()
    return 0 if success else 1

if __name__ == '__main__':
    sys.exit(main())
