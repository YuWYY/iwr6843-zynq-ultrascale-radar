#!/usr/bin/env python3
"""Offline ELF64/AArch64 SW4 layout and optional frozen hardware-file check.

Uses only Python's standard library. Does NOT contact hardware, inspect loaded
PL configuration, prove cache attributes at runtime, or replace a native build.
The application repeats translation/stack/quiescence checks on cold boot.
"""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

EXPECTED_XSA = 'b55a70c587112845460c9ef8bf0e646c21df0b67c7a313f8c012bdbb1bf9f01a'
EXPECTED_BIT = 'b2493f5edeb8b117c0b285622f5172e9abc30305eac9f09e5a00cf4a8d2622e5'
VERSION = b'M2-SW4.2-v1.12.0'

class BuildError(ValueError):
    pass

def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()

def elf_details(raw: bytes) -> tuple[dict[str, int], list[dict[str, int]], int]:
    if len(raw) < 64 or raw[:7] != b'\x7fELF\x02\x01\x01':
        raise BuildError('requires ELF64 little-endian version 1, not a bin/map or host ELF')
    h = struct.unpack_from('<HHIQQQIHHHHHH', raw, 16)
    etype, machine, ver, entry, phoff, shoff, _, ehsize, phsize, phnum, shsize, shnum, _ = h
    if etype != 2 or machine != 183 or ver != 1 or ehsize != 64:
        raise BuildError('requires executable AArch64 ELF, e_machine=183')
    if phsize != 56 or shsize != 64 or not 0 < phnum < 4096 or not 0 < shnum < 65535:
        raise BuildError('unsupported/missing ELF table sizes; retain symbols, do not strip')
    def block(offset: int, size: int) -> bytes:
        if offset < 0 or size < 0 or offset + size > len(raw):
            raise BuildError('truncated/out-of-range ELF table')
        return raw[offset:offset+size]
    sections = [struct.unpack('<IIQQQQIIQQ', block(shoff+i*64, 64)) for i in range(shnum)]
    symbols: dict[str, int] = {}
    for sec in sections:
        _, typ, _, _, offset, length, linked, _, _, entsize = sec
        if typ != 2:  # SHT_SYMTAB, not only dynamic symbols
            continue
        if entsize != 24 or length % 24 or linked >= shnum:
            raise BuildError('invalid symbol table')
        ss = sections[linked]
        names = block(ss[4], ss[5])
        table = block(offset, length)
        for i in range(0, length, 24):
            no, _, _, idx, value, _ = struct.unpack_from('<IBBHQQ', table, i)
            if idx == 0 or no == 0:
                continue
            if no >= len(names) or names.find(b'\0', no) < 0:
                raise BuildError('invalid symbol name')
            name = names[no:names.index(0, no)].decode('utf-8', 'replace')
            if name in symbols and symbols[name] != value and name.startswith('__radar_'):
                raise BuildError('conflicting layout symbol: ' + name)
            symbols[name] = value
    programs=[]
    for i in range(phnum):
        typ, flags, offset, va, pa, filesz, memsz, align = struct.unpack('<IIQQQQQQ', block(phoff+i*56, 56))
        if filesz > memsz or offset + filesz > len(raw):
            raise BuildError('invalid/truncated program segment')
        if typ == 1:
            programs.append(dict(address=va, physical=pa, file_bytes=filesz, memory_bytes=memsz, flags=flags, alignment=align))
    return symbols, programs, entry

def validate_layout(symbols: dict[str, int], programs: list[dict[str, int]], entry: int) -> dict:
    required = ('__radar_ddr_start','__radar_ddr_end','__radar_normal_end',
                '__radar_bd_zone_start','__radar_bd_zone_end',
                '__radar_rx_pool_start','__radar_rx_pool_end',
                '__radar_capture_store_start','__radar_capture_store_end',
                '_heap_start','_heap_end','_el3_stack_end','__el3_stack','__mmu_tbl2_end',
                'radar_session_poll','radar_memory_prepare',
                '__radar_display_frames_start','__radar_display_frames_end',
                '__radar_display_control_start','__radar_display_control_end','radar_display_poll')
    required_dsp=('__radar_dsp_workspace_start','__radar_dsp_workspace_end','radar_dsp_submit','radar_dsp_poll')
    missing = [k for k in required+required_dsp if k not in symbols]
    if missing:
        raise BuildError('missing SW4/linker symbols: ' + ', '.join(missing))
    d0,d1,normal,b0,b1,r0,r1,c0,c1 = (symbols[k] for k in required[:9])
    if not 0 <= d0 <= entry < normal <= b0 < b1 == r0 < r1 == c0 < c1 <= min(d1,0x7ff00000):
        raise BuildError('invalid DDR/entry/normal/BD/RX/result order or overlap')
    if b0 % 0x200000 or b1-b0 != 0x200000 or r1-r0 != 0x800000 or c1-c0 != 0x800000:
        raise BuildError('requires dedicated aligned 2MiB BD + 8MiB RX + 8MiB CPU store')
    f0,f1,dc0,dc1=(symbols[k] for k in required[-5:-1])
    if (f0,f1,dc0,dc1)!=(0x1400000,0x2400000,0x2400000,0x2600000) or c1!=f0 or dc1>d1:
        raise BuildError('requires nonoverlapping 16MiB DP frames + 2MiB DP control after CPU store')
    w0,w1=(symbols[k] for k in required_dsp[:2])
    if (w0,w1)!=(0x02600000,0x03600000) or normal>0x00200000 or w1>d1:
        raise BuildError('requires dedicated16MiB DSP workspace with ordinary image below2MiB')
    for begin,end,minimum in (('_heap_start','_heap_end',65536),('_el3_stack_end','__el3_stack',16384)):
        if not d0 <= symbols[begin] < symbols[end] <= normal or symbols[end]-symbols[begin] < minimum:
            raise BuildError('invalid/undersized heap or EL3 stack: ' + begin)
    if not d0 < symbols['__mmu_tbl2_end'] <= normal:
        raise BuildError('MMU table extends into reserved buffers')
    if any(not d0 <= symbols[k] < normal for k in ('radar_session_poll','radar_memory_prepare','radar_display_poll','radar_dsp_submit','radar_dsp_poll')):
        raise BuildError('application functions outside normal image')
    if not programs or not any(p['flags'] & 1 and p['address'] <= entry < p['address']+p['memory_bytes'] for p in programs):
        raise BuildError('entry is not in an executable PT_LOAD')
    for p in programs:
        if p['address'] != p['physical']:
            raise BuildError('nonidentity load address; this runtime requires EL3 identity-map BSP')
        if p['file_bytes'] and not d0 <= p['address'] < p['address']+p['file_bytes'] <= b0:
            raise BuildError('file-backed data overlaps reserved NOLOAD zones or leaves application DDR')
    return dict(entry=f'0x{entry:x}', normal_end=f'0x{normal:x}',
                bd=[f'0x{b0:x}',f'0x{b1:x}'],rx=[f'0x{r0:x}',f'0x{r1:x}'],
                cpu_store=[f'0x{c0:x}',f'0x{c1:x}'],
                display_frames=[f'0x{f0:x}',f'0x{f1:x}'],display_control=[f'0x{dc0:x}',f'0x{dc1:x}'],
                dsp_workspace=[f'0x{w0:x}',f'0x{w1:x}'],
                heap_bytes=symbols['_heap_end']-symbols['_heap_start'],
                el3_stack_bytes=symbols['__el3_stack']-symbols['_el3_stack_end'])

def inspect_elf(raw: bytes) -> dict:
    symbols, programs, entry = elf_details(raw)
    if VERSION not in raw:
        raise BuildError('SW4 version string missing; reject old SW3/stale executable')
    return dict(status='OFFLINE_ELF_LAYOUT_PASS',elf_sha256=sha(raw),
                layout=validate_layout(symbols,programs,entry),
                hardware_validation='NOT_RUN',note='File evidence only; not native-build or board proof')

def main() -> int:
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--elf',type=Path,required=True)
    ap.add_argument('--xsa',type=Path)
    ap.add_argument('--bit',type=Path)
    ap.add_argument('--output',type=Path)
    a=ap.parse_args()
    try:
        result=inspect_elf(a.elf.read_bytes())
        for key,path,expected in (('xsa',a.xsa,EXPECTED_XSA),('bit',a.bit,EXPECTED_BIT)):
            if path:
                actual=sha(path.read_bytes())
                if actual!=expected:
                    raise BuildError(f'{key} does not match frozen M2 baseline: {actual}')
                result[key+'_sha256']=actual
        text=json.dumps(result,ensure_ascii=False,indent=2)+'\n'
        if a.output:
            a.output.parent.mkdir(parents=True,exist_ok=True)
            a.output.write_text(text,encoding='utf-8')
        print(text,end='');return 0
    except (BuildError,OSError,struct.error) as exc:
        print('ERROR: '+str(exc),file=sys.stderr);return 2

if __name__=='__main__':
    raise SystemExit(main())
