#!/usr/bin/env python3
"""Compile app translation units with Clang's AArch64 backend and supplied BSP.

This is an object/interface check, NOT native Vitis linking or board validation.
Libc declaration-only shims are used because the native GCC sysroot is absent.
No shims are copied to target src. Objects are verification artifacts, not ELF.
"""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys

def main() -> int:
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--bsp',required=True,type=Path,help='directory containing include/xparameters.h')
    ap.add_argument('--clang',default='clang')
    ap.add_argument('--output',required=True,type=Path)
    a=ap.parse_args();root=Path(__file__).resolve().parents[1]
    try:
        include=a.bsp.resolve()/'include';compiler=shutil.which(a.clang)
        if not compiler or not (include/'xparameters.h').is_file():
            raise ValueError('Clang or actual supplied BSP/include/xparameters.h unavailable')
        a.output.mkdir(parents=True,exist_ok=True)
        base=[compiler,'--target=aarch64-none-elf','-ffreestanding','-O2','-g','-std=gnu11','-Wall','-Wextra','-Werror','-U__clang__','-DSDT','-DEL3=1',
              '-I'+str(root/'tests/aarch64_libc_declarations'),'-I'+str(include),*['-I'+str(root/'radar_runtime/src'/part) for part in ('app','hal','capture','config')]]
        # GNU-compatible inline-asm branch matches this GNU-built BSP. Actual
        # Armclang headers are absent; -U__clang__ is local to this check only.
        results=[]
        for source in sorted((root/'radar_runtime/src').rglob('*.c')):
            cmd=base+['-c',str(source),'-o',str(a.output/(source.stem+'.o'))]
            run=subprocess.run(cmd,text=True,capture_output=True)
            results.append(dict(file=str(source.relative_to(root/'radar_runtime/src')),source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),command=cmd,returncode=run.returncode,stdout=run.stdout,stderr=run.stderr))
            print(('PASS ' if run.returncode==0 else 'FAIL ')+source.name)
            if run.stdout or run.stderr:print(run.stdout+run.stderr)
        ok=len(results)==14 and all(x['returncode']==0 for x in results)
        report=dict(status='AARCH64_OBJECT_INTERFACE_PASS' if ok else 'FAIL',
            compiler=subprocess.run([compiler,'--version'],capture_output=True,text=True).stdout.splitlines()[0],
            native_vitis_build='NOT_RUN',board='NOT_RUN',
            libc='DECLARATIONS_ONLY_TEST_SHIM_NOT_RUNTIME',results=results)
        (a.output/'aarch64_interfaces.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
        print(f'{len(results)} translation units; native Vitis link and board run NOT performed')
        return 0 if ok else 1
    except (OSError,ValueError) as exc:
        print('ERROR: '+str(exc),file=sys.stderr);return 2

if __name__=='__main__':raise SystemExit(main())
