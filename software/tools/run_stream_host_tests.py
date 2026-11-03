#!/usr/bin/env python3
"""Run targeted production STREAM/FINITE host tests with GCC+UBSan (Linux/WSL).

Does not access UART/JTAG, build FPGA or write target firmware. Source hashes,
exact argv, compiler/runtime logs and a non-promoted HOST_ONLY result are saved.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
APP = ROOT / 'radar_runtime/src'
BASE = [
    'hal/radar_control.c','hal/radar_cli.c','app/radar_console.c','app/radar_sw2.c','app/radar_sw3.c',
    'hal/radar_dma.c','capture/radar_capture_profile.c','capture/radar_snapshot.c','capture/radar_session.c',
    'capture/radar_crc32.c','capture/radar_export.c','hal/radar_dsp.c','hal/radar_dsp_platform.c',
]
MODEL = ['tests/model.c','tests/vendor_amd/xaxidma.c','tests/vendor_amd/xaxidma_bd.c','tests/vendor_amd/xaxidma_bdring.c']
def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--only',action='append',choices=('stream_capture','stream_scan','finite_profile','runtime','scene_record'),help='Run only selected focused production tests')
    args = ap.parse_args()
    out = args.output.resolve(); out.mkdir(parents=True, exist_ok=True)
    build = Path(tempfile.mkdtemp(prefix='radar-stream-tests-'))
    includes = [ROOT/'tests',ROOT/'tests/mock_bsp',ROOT/'tests/vendor_amd'] + [APP/x for x in ('app','hal','capture','config','processing','display')]
    flags = ['gcc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-UNDEBUG','-DSDT',
             '-fsanitize=undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer'] + [f'-I{x}' for x in includes]
    base = [APP/x for x in BASE]+[ROOT/x for x in MODEL]
    sources = {
        'stream_capture': [ROOT/'tests/test_stream_capture.c'] + base + [APP/x for x in ('capture/radar_stream.c','capture/radar_raw_slots.c','processing/radar_range.c')],
        'stream_scan': [ROOT/'tests/test_stream_scan.c'] + [APP/x for x in ('processing/radar_stream_scan.c','processing/radar_scene_record.c','processing/radar_range.c','processing/radar_spatial.c','processing/radar_scene.c','processing/radar_reflection.c','processing/radar_freshness.c','capture/radar_raw_slots.c','capture/radar_capture_profile.c','capture/radar_crc32.c','hal/radar_dsp.c','hal/radar_time_bridge.c')],
        'finite_profile': [ROOT/'tests/test_finite_profile.c'] + base,
        'runtime': [ROOT/'tests/test_runtime.c'] + base,
        'scene_record': [ROOT/'tests/test_scene_record.c',APP/'processing/radar_scene_record.c',APP/'capture/radar_crc32.c'],
    }
    if args.only:
        sources={key:value for key,value in sources.items() if key in args.only}
    def snapshot():
        paths = {p for group in sources.values() for p in group}
        for folder in (APP/'capture', APP/'processing', APP/'hal', APP/'config', ROOT/'tests'):
            paths.update(folder.rglob('*.h'))
        return {str(p.relative_to(ROOT)):sha(p) for p in sorted(paths)}
    before = snapshot()
    results = {}
    def run_test(item):
        name, files = item
        cmd = flags + [str(p) for p in files] + ['-lm','-o',str(build/name)]
        (out/f'{name}.command.json').write_text(json.dumps(cmd,indent=2)+'\n')
        compiled = subprocess.run(cmd,cwd=ROOT,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=180)
        (out/f'{name}.build.log').write_text(compiled.stdout)
        result = {'build_exit': compiled.returncode, 'run_exit': None}
        if compiled.returncode == 0:
            env=dict(os.environ)
            if name=='scene_record':
                env['RADAR_RECORD_FIXTURE']=str(out/'scene_record_fixture.bin')
            if name=='stream_scan':
                env['RADAR_STREAM_SCENE_FIXTURE']=str(out/'stream_scene_fixture.bin')
            run = subprocess.run([str(build/name)],cwd=ROOT,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=90,env=env)
            (out/f'{name}.run.log').write_text(run.stdout)
            result['run_exit'] = run.returncode
        return name,result
    with ThreadPoolExecutor(max_workers=2) as pool:
        for name,result in pool.map(run_test,sources.items()):
            results[name] = result
            print(name,result,flush=True)
    python_tests=['test_stream_profile.py','test_finite_profile.py','test_scene_record_decoder.py'] if not args.only else (['test_scene_record_decoder.py'] if 'scene_record' in args.only else [])
    for name in python_tests:
        env=dict(os.environ);env['RADAR_RECORD_FIXTURE']=str(out/'scene_record_fixture.bin')
        run = subprocess.run(['python3',str(ROOT/'tests'/name)],cwd=ROOT,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=60,env=env)
        (out/f'{name}.log').write_text(run.stdout)
        results[name] = {'run_exit':run.returncode}
    passed = all(x.get('build_exit',0)==0 and x['run_exit']==0 for x in results.values())
    hashes = snapshot()
    changed = [p for p in sorted(set(before)|set(hashes)) if before.get(p)!=hashes.get(p)]
    passed = passed and not changed
    summary = {'scope':'HOST_ONLY_PRODUCTION_SOFTWARE_AND_AMD_DRIVER_MODEL',
               'physical_or_rtl_timing_evidence':False,'ubsan':True,'passed':passed,'tests':results,
               'build_directory':str(build),'source_stable':not changed,'changed_during_run':changed,
               'source_sha256':before,'source_after_sha256':hashes}
    (out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
    return 0 if passed else 1

if __name__=='__main__':
    raise SystemExit(main())
