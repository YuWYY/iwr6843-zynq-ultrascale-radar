#!/usr/bin/env python3
"""Standard-library regression tests. All firmware/cfg fixtures are HOST-ONLY.
No synthetic fixture is installed into target src or certified for a radar.
"""
from __future__ import annotations
import copy
import hashlib
import importlib.util
import json
import os
import re
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

ROOT=Path(__file__).resolve().parents[1]
def load(name):
    spec=importlib.util.spec_from_file_location(name,ROOT/'tools'/(name+'.py'))
    module=importlib.util.module_from_spec(spec);sys.modules[name]=module
    spec.loader.exec_module(module);return module
G=load('generate_capture_profile');D=load('decode_capture');B=load('check_build');I=load('install_sources')
CFG=b'''% HOST-ONLY fixture. NOT verified for IWR6843 hardware.
sensorStop
flushCfg
dfeDataOutputMode 1
channelCfg 15 3 0
adcCfg 2 1
adcbufCfg -1 0 0 1 1
profileCfg 0 60 7 3 57 0 0 70 1 256 5000 0 0 30
chirpCfg 0 0 0 0 0 0 0 1
frameCfg 0 0 128 0 100 1 0
lvdsStreamCfg -1 0 1 0
sensorStart
'''
FW=b'NOT_RADAR_FIRMWARE: host-only byte identity fixture\x00\xff'
def review(cfg=CFG):
    r=json.loads((ROOT/'docs'/'capture_review.template.json').read_text())
    r.update(name='HOST_ONLY_NOT_A_HARDWARE_PROFILE',firmware_sha256=G.sha(FW),cfg_sha256=G.sha(cfg),version_substring='03.06.02.00')
    for v in r['approvals'].values():
        v.update(approved=True,evidence='HOST TEST fixture only, no actual board validation')
    return r

def generated(r=None,cfg=CFG,review_dir=None):
    return G.generate(FW,cfg,json.dumps(review(cfg) if r is None else r).encode(),review_dir)

def trial_review(directory):
    """HOST ONLY: a bounded trial with explicitly unverified dynamic conditions."""
    r=review();r.update(qualification='FIRST_PACKET_TEST',startup_mode='REVIEWED_PREAMBLE')
    for index,item in enumerate(r['approvals'].values()):
        item['approved']=bool(G.TRIAL_APPROVAL_MASK & (1 << index))
    r['source_reviews']={}
    for key in G.SOURCE_REVIEWS:
        path=directory/(key+'.txt');path.write_bytes(('HOST SOURCE REVIEW FIXTURE '+key).encode())
        r['source_reviews'][key]={'approved':True,'summary':'HOST ONLY source compatibility fixture, not actual measurement',
                                  'evidence':[{'path':path.name,'sha256':G.sha(path.read_bytes())}]}
    return r

def serial(payload=bytes(range(32)),sid=7):
    md={k:'0' for k in D.MANDATORY}
    for key in md:
        if 'sha256' in key: md[key]='a'*64
    md.update(software='M2-SW4.1-v1.1',profile_id='123456789abcdef0',geometry=f'{len(payload)} 1 8 0 1',transport='RAW16_HOST_FIXTURE_ONLY')
    crc=zlib.crc32(payload)
    lines=[f'@R4BEGIN {sid} {len(payload)} {crc:08x}']
    lines += [f'@R4META {k} {v}' for k,v in sorted(md.items())]
    lines += [f'@R4DATA {sid} {i} {payload[i:i+128].hex()}' for i in range(0,len(payload),128)]
    lines += [f'@R4END {sid} {len(payload)} {crc:08x}']
    return '\n'.join(lines)+'\n'

def symbols():
    return dict(__radar_ddr_start=0,__radar_ddr_end=0x7ff00000,__radar_normal_end=0x50000,
        __radar_bd_zone_start=0x200000,__radar_bd_zone_end=0x400000,
        __radar_rx_pool_start=0x400000,__radar_rx_pool_end=0xc00000,
        __radar_capture_store_start=0xc00000,__radar_capture_store_end=0x1400000,
        _heap_start=0x20000,_heap_end=0x30000,_el3_stack_end=0x30000,__el3_stack=0x34000,
        __mmu_tbl2_end=0x18000,radar_session_poll=0x1000,radar_memory_prepare=0x2000,
        __radar_display_frames_start=0x1400000,__radar_display_frames_end=0x2400000,
        __radar_display_control_start=0x2400000,__radar_display_control_end=0x2600000,
        radar_display_poll=0x3000,
        __radar_dsp_workspace_start=0x2600000,__radar_dsp_workspace_end=0x3600000,
        radar_dsp_submit=0x4000,radar_dsp_poll=0x5000)

def synthetic_elf():
    # Minimal parser fixture, never delivered as a target program.
    s=symbols();names=b'\0';table=b'\0'*24
    for name,value in s.items():
        off=len(names);names+=name.encode()+b'\0'
        table+=struct.pack('<IBBHQQ',off,0x10,0,0xfff1,value,0)
    raw=bytearray(0x1800)
    raw[:16]=b'\x7fELF\x02\x01\x01'+b'\0'*9
    raw[16:64]=struct.pack('<HHIQQQIHHHHHH',2,183,1,0,64,0x100,0,64,56,1,64,3,0)
    raw[64:120]=struct.pack('<IIQQQQQQ',1,5,0x1000,0,0,64,0x1400000,4096)
    raw[0x140:0x180]=struct.pack('<IIQQQQIIQQ',0,3,0,0,0x400,len(names),0,0,1,0)
    raw[0x180:0x1c0]=struct.pack('<IIQQQQIIQQ',0,2,0,0,0x800,len(table),1,0,8,24)
    raw[0x400:0x400+len(names)]=names;raw[0x800:0x800+len(table)]=table
    raw[0x1000:0x1000+len(B.VERSION)]=B.VERSION
    return bytes(raw)

class ProfileTests(unittest.TestCase):
    def test_default_unqualified(self):
        r=json.loads((ROOT/'docs/capture_review.template.json').read_text())
        self.assertTrue(all(v['approved'] is False for v in r['approvals'].values()))
        r.update(firmware_sha256=G.sha(FW),cfg_sha256=G.sha(CFG))
        with self.assertRaises(G.ProfileError): generated(r)
    def test_valid_identity_and_command_separation(self):
        h,m=generated();self.assertEqual(m['total_bytes'],131072)
        table=h.split('static const RadarCaptureProfile')[0]
        self.assertNotIn('sensorStart',table);self.assertNotIn('sensorStop',table)
        self.assertIn('.start_command="sensorStart"',h)
        self.assertEqual(m['firmware_sha256'],G.sha(FW))
        r=review();r['name']+='X';self.assertNotEqual(generated(r)[1]['profile_id'],m['profile_id'])
    def test_hash_and_evidence_guard(self):
        for change in ('hash','unapproved','evidence','schema'):
            r=review()
            if change=='hash': r['cfg_sha256']='0'*64
            if change=='unapproved': r['approvals'][G.APPROVALS[0]]['approved']=False
            if change=='evidence': r['approvals'][G.APPROVALS[1]]['evidence']='yes'
            if change=='schema': r['schema']=True
            with self.subTest(change=change),self.assertRaises(G.ProfileError):generated(r)
    def test_invalid_geometry_and_types(self):
        for key,value in [('packet_bytes',131073),('packet_bytes',True),('packet_bytes',8388608),('packet_count',65),('packet_count',1.0),('lanes',4),('lane_mbps',450),('format_flags',32),('receive_timeout_us',5000001),('skip_pairs',-1)]:
            r=review();r[key]=value
            with self.subTest(key=key,value=value),self.assertRaises(G.ProfileError):generated(r)
    def test_descriptor_limit_separate_from_total_size(self):
        r=review();r.update(packet_bytes=65544,packet_count=127)
        self.assertLess(r['packet_bytes']*r['packet_count'],8388608)
        with self.assertRaises(G.ProfileError):generated(r)
        r.update(packet_bytes=131072,packet_count=64)
        self.assertEqual(generated(r)[1]['total_bytes'],8388608)
    def test_reject_injection_partial_configuration_and_start_alias(self):
        for raw in (CFG+b'flushCfg\n',CFG.replace(b'sensorStart\n',b'sensorStart 0\n'),CFG.replace(b'channelCfg 15 3 0',b'channelCfg 15 3 0; sensorStart'),b'flushCfg\nsensorStart\n',CFG.replace(b'flushCfg\n',b'flushCfg\nflushCfg\n'),CFG.replace(b'lvdsStreamCfg -1 0 1 0',b'system reboot')):
            with self.subTest(raw=raw[:40]),self.assertRaises(G.ProfileError):generated(cfg=raw)
    def test_preamble_delay_and_version(self):
        r=review();r.update(startup_mode='REVIEWED_PREAMBLE',use_delay=True)
        with self.assertRaises(G.ProfileError):generated(r)
        r['use_delay']=False;self.assertIn('.startup_mode=2U',generated(r)[0])
        r['version_substring']='mmwDemo';self.assertRaises(G.ProfileError,generated,r)
    def test_c_allowlist_matches_generator(self):
        code=(ROOT/'radar_runtime/src/capture/radar_capture_profile.c').read_text()
        for verb in G.VERBS:self.assertIn('"'+verb+'"',code)
    def test_cli_tool_writes_no_automatic_hardware_actions(self):
        with tempfile.TemporaryDirectory() as t:
            p=Path(t);(p/'fw.bin').write_bytes(FW);(p/'radar.cfg').write_bytes(CFG);(p/'review.json').write_text(json.dumps(review()))
            args=[sys.executable,str(ROOT/'tools/generate_capture_profile.py'),'--firmware',str(p/'fw.bin'),'--cfg',str(p/'radar.cfg'),'--review',str(p/'review.json'),'--output',str(p/'out.h')]
            self.assertEqual(subprocess.run(args,capture_output=True).returncode,0)
            self.assertTrue((p/'out.manifest.json').exists())
            self.assertEqual(subprocess.run(args,capture_output=True).returncode,2)
    def test_trial_preserves_pending_approvals_and_has_distinct_identity(self):
        with tempfile.TemporaryDirectory() as t:
            p=Path(t);r=trial_review(p);h,m=generated(r,review_dir=p)
            self.assertEqual(m['approvals'],0x83);self.assertEqual(m['source_review_flags'],7)
            self.assertEqual(m['qualification'],'FIRST_PACKET_TEST')
            self.assertEqual(len(m['source_evidence']),3)
            self.assertIn('.approvals=0x00000083U',h)
            self.assertIn('.qualification=1U',h)
            full=copy.deepcopy(r);full['qualification']='QUALIFIED'
            for item in full['approvals'].values():item['approved']=True
            _,fm=generated(full,review_dir=p)
            self.assertEqual(fm['qualification'],'QUALIFIED');self.assertEqual(fm['approvals'],255)
            self.assertNotEqual(m['profile_id'],fm['profile_id'])
    def test_trial_never_waives_electrical_firmware_or_geometry_approval(self):
        with tempfile.TemporaryDirectory() as t:
            p=Path(t);base=trial_review(p)
            for index in (0,1,7):
                r=copy.deepcopy(base);r['approvals'][G.APPROVALS[index]]['approved']=False
                with self.subTest(index=index),self.assertRaisesRegex(G.ProfileError,'unapproved:'):
                    generated(r,review_dir=p)
            r=copy.deepcopy(base);r.pop('qualification')
            with self.assertRaisesRegex(G.ProfileError,'unapproved:'):generated(r,review_dir=p)
    def test_trial_requires_all_bound_local_source_reviews(self):
        with tempfile.TemporaryDirectory() as t:
            p=Path(t);base=trial_review(p)
            with self.assertRaisesRegex(G.ProfileError,'review file directory'):generated(base)
            for key in G.SOURCE_REVIEWS:
                for failure in ('unapproved','no_evidence','missing','digest','summary','network'):
                    r=copy.deepcopy(base);item=r['source_reviews'][key]
                    if failure=='unapproved':item['approved']=False
                    if failure=='no_evidence':item['evidence']=[]
                    if failure=='missing':item['evidence'][0]['path']='absent.txt'
                    if failure=='digest':item['evidence'][0]['sha256']='0'*64
                    if failure=='summary':item['summary']='yes'
                    if failure=='network':item['evidence'][0]['path']='https://example.invalid/source'
                    with self.subTest(key=key,failure=failure),self.assertRaises(G.ProfileError):
                        generated(r,review_dir=p)
            (p/'wire_format.txt').write_text('changed source evidence')
            with self.assertRaisesRegex(G.ProfileError,'SHA-256 mismatch'):generated(base,review_dir=p)
    def test_trial_rejects_every_scope_expansion(self):
        with tempfile.TemporaryDirectory() as t:
            p=Path(t);base=trial_review(p)
            for key,value in [('packet_bytes',65536),('packet_count',2),('format_flags',0),('skip_pairs',1),
                              ('use_delay',True),('startup_mode','IDLE_CLOCK'),('lanes',4),
                              ('lane_mbps',450),('word_bits',14),('qualification','TRUST_ME')]:
                r=copy.deepcopy(base);r[key]=value
                with self.subTest(key=key),self.assertRaises(G.ProfileError):generated(r,review_dir=p)
    def test_trial_cli_resolves_evidence_relative_to_review_not_cwd(self):
        with tempfile.TemporaryDirectory() as t:
            p=Path(t);reviews=p/'reviews';reviews.mkdir();r=trial_review(reviews)
            (p/'fw.bin').write_bytes(FW);(p/'radar.cfg').write_bytes(CFG)
            (reviews/'review.json').write_text(json.dumps(r))
            args=[sys.executable,str(ROOT/'tools/generate_capture_profile.py'),'--firmware',str(p/'fw.bin'),
                  '--cfg',str(p/'radar.cfg'),'--review',str(reviews/'review.json'),'--output',str(p/'trial.h')]
            result=subprocess.run(args,capture_output=True,text=True,cwd=p)
            self.assertEqual(result.returncode,0,result.stderr)
            self.assertEqual(json.loads((p/'trial.manifest.json').read_text())['approvals'],0x83)

class DecodeTests(unittest.TestCase):
    def test_new_export_cannot_fall_back_to_legacy_scope(self):
        text=serial().replace('M2-SW4.1-v1.1','M2-SW4.1-v1.2')
        fields=['@R4META qualification QUALIFIED', '@R4META review_bits ff 00',
                '@R4META validation_scope PROFILE_REVIEWED_NOT_LIVE_MEASUREMENT']
        def add(entries):
            return text.replace('@R4DATA ', '\n'.join(entries)+'\n@R4DATA ',1)
        self.assertEqual(D.captures_from_log(add(fields))[0].metadata['qualification'],'QUALIFIED')
        for missing in range(len(fields)):
            with self.subTest(missing=fields[missing]), self.assertRaises(D.DecodeError):
                D.captures_from_log(add([x for i,x in enumerate(fields) if i!=missing]))
    def test_valid_crc_and_metadata_and_save(self):
        x=D.captures_from_log(serial());self.assertEqual(bytes(x[0].payload),bytes(range(32)))
        with tempfile.TemporaryDirectory() as t:
            b,j=D.save_capture(x[0],Path(t)/'result')
            self.assertEqual(b.read_bytes(),bytes(range(32)));self.assertEqual(json.loads(j.read_text())['adc_semantics'],'NOT_VERIFIED')
            with self.assertRaises(D.DecodeError):D.save_capture(x[0],Path(t)/'result')
    def test_timestamp_prefix_and_interrupted_restart(self):
        full=serial(bytes(range(256)));broken=full[:full.index('@R4DATA 7 128')]
        x=D.captures_from_log(broken+'other log\n'+''.join('[00:01.000] '+l+'\n' for l in full.splitlines()))
        self.assertEqual(len(x),1);self.assertEqual(bytes(x[0].payload),bytes(range(256)))
    def test_trailing_incomplete_does_not_reuse_previous_success(self):
        for sid in (7,8):
            later=serial(bytes(range(256)),sid=sid)
            for cut in ('@R4META','@R4DATA',f'@R4DATA {sid} 128','@R4END'):
                with self.subTest(session=sid,cut=cut),self.assertRaisesRegex(D.DecodeError,'incomplete trailing export'):
                    D.captures_from_log(serial()+later[:later.index(cut)])
        with tempfile.TemporaryDirectory() as t:
            p=Path(t);later=serial(sid=8)
            (p/'log.txt').write_text(serial()+later[:later.index('@R4END')])
            args=[sys.executable,str(ROOT/'tools/decode_capture.py'),str(p/'log.txt'),'--out',str(p/'r')]
            result=subprocess.run(args,capture_output=True,text=True)
            self.assertEqual(result.returncode,2,result.stdout)
            self.assertIn('incomplete trailing export',result.stderr)
            self.assertFalse((p/'r.bin').exists());self.assertFalse((p/'r.json').exists())
    def test_corruption_missing_duplicate_and_reordered_bytes(self):
        log=serial(bytes(range(256)))
        record=next(l for l in log.splitlines() if '@R4DATA' in l)
        for bad in (log.replace(record,record.replace('000102','100102')),log.replace(record,'',1),log.replace(record,record+'\n'+record),log.replace('@R4DATA 7 128','@R4DATA 7 129'),log.replace('@R4END 7 256','@R4END 8 256')):
            with self.subTest(log=bad[-60:]),self.assertRaises(D.DecodeError):D.captures_from_log(bad)
    def test_metadata_schema_and_incomplete_export(self):
        log=serial()
        for bad in (log[:log.index('@R4END')],log.replace('@R4META profile_id 123456789abcdef0\n',''),log.replace('@R4META geometry 32 1 8 0 1','@R4META geometry 16 1 8 0 1'),log.replace('@R4META profile_id 123456789abcdef0','@R4META profile_id 123456789abcdef0\n@R4META profile_id abcdef1234567890')):
            with self.assertRaises(D.DecodeError):D.captures_from_log(bad)
    def test_multiple_exports_require_selection(self):
        with tempfile.TemporaryDirectory() as t:
            p=Path(t);(p/'log.txt').write_text(serial()+serial(sid=8))
            args=[sys.executable,str(ROOT/'tools/decode_capture.py'),str(p/'log.txt'),'--out',str(p/'r')]
            self.assertEqual(subprocess.run(args,capture_output=True).returncode,2)
            self.assertEqual(subprocess.run(args+['--session','8'],capture_output=True).returncode,0)
    def test_same_session_reexports_require_explicit_occurrence(self):
        with tempfile.TemporaryDirectory() as t:
            p=Path(t);second=bytes(reversed(range(32)))
            (p/'log.txt').write_text(serial()+serial(second))
            args=[sys.executable,str(ROOT/'tools/decode_capture.py'),str(p/'log.txt'),'--out',str(p/'r')]
            for selection in ([],['--session','7'],['--session','7','--occurrence','3']):
                with self.subTest(selection=selection):
                    self.assertEqual(subprocess.run(args+selection,capture_output=True).returncode,2)
                    self.assertFalse((p/'r.bin').exists())
            result=subprocess.run(args+['--session','7','--occurrence','2'],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            self.assertEqual((p/'r.bin').read_bytes(),second)
            meta=json.loads((p/'r.json').read_text())
            self.assertEqual(meta['session'],7);self.assertEqual(meta['crc32'],f'{zlib.crc32(second):08x}')
    def test_real_production_export_roundtrip(self):
        path=os.getenv('RADAR_TEST_EXPORT_LOG')
        if not path:self.skipTest('run via CTest for actual C exporter artifact')
        log=Path(path).read_text();captures=D.captures_from_log(log)
        self.assertEqual(len(captures),1);out=captures[0]
        expected=bytes((i*17+11)&255 for i in range(131072))
        self.assertEqual(out.size,131072)
        self.assertEqual(bytes(out.payload),expected)
        self.assertEqual(out.crc32,zlib.crc32(expected))
        version_header=(ROOT/'radar_runtime/src/config/radar_contract.h').read_text()
        expected_version=re.search(r'#define\s+RADAR_SW_VERSION\s+"([^"]+)"',version_header)
        self.assertIsNotNone(expected_version)
        self.assertEqual(out.metadata['software'],expected_version[1])
        self.assertEqual(out.metadata['xsa_sha256_expected'],B.EXPECTED_XSA)
        self.assertEqual(out.metadata['bit_sha256_expected'],B.EXPECTED_BIT)
        records=[line for line in log.splitlines() if line.startswith('@R4DATA ')]
        self.assertEqual(len(records),1024)
        self.assertTrue(all(len(line.split()[3])==256 for line in records))
        with tempfile.TemporaryDirectory() as t:
            p=Path(t)
            # Match the user's monitor: timestamp/header on a separate line,
            # complete ASCII records underneath, with CRLF preserved.
            monitored=''.join('[2026-10-29 17:35:18.258]# RECV ASCII>\r\n'+line+'\r\n\r\n' for line in log.splitlines())
            (p/'monitor.txt').write_bytes(monitored.encode('utf-8'))
            args=[sys.executable,str(ROOT/'tools/decode_capture.py'),str(p/'monitor.txt'),'--out',str(p/'capture')]
            result=subprocess.run(args,capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            self.assertEqual((p/'capture.bin').read_bytes(),expected)
            meta=json.loads((p/'capture.json').read_text())
            self.assertEqual(meta['bytes'],131072);self.assertEqual(meta['crc32'],f'{zlib.crc32(expected):08x}')
            self.assertEqual(meta['metadata'],out.metadata)
            self.assertEqual(meta['validation'],'SERIAL_EXPORT_LENGTH_CRC_ONLY')
            self.assertEqual(meta['adc_semantics'],'NOT_VERIFIED')

class BuildTests(unittest.TestCase):
    def test_elf64_symbols_layout_zero_entry(self):
        r=B.inspect_elf(synthetic_elf());self.assertEqual(r['status'],'OFFLINE_ELF_LAYOUT_PASS');self.assertEqual(r['layout']['entry'],'0x0')
    def test_non_aarch64_truncation_and_old_version_rejected(self):
        good=synthetic_elf();bad=bytearray(good);bad[18:20]=struct.pack('<H',62)
        for raw in (bytes(bad),good[:24],good[:1000],good.replace(B.VERSION,b'M2-SW3-v1.0')):
            with self.subTest(size=len(raw)),self.assertRaises(B.BuildError):B.inspect_elf(raw)
    def test_overlap_heap_stack_and_noload(self):
        good=synthetic_elf();sy,pr,en=B.elf_details(good)
        for key,value in [('__radar_capture_store_start',0x400000),('__radar_bd_zone_end',0x300000),('_heap_end',0x20010),('__el3_stack',0x30040),('__radar_capture_store_end',0x80000000)]:
            bad=sy.copy();bad[key]=value
            with self.subTest(key=key),self.assertRaises(B.BuildError):B.validate_layout(bad,pr,en)
        pr[0]['file_bytes']=0x400001
        self.assertRaises(B.BuildError,B.validate_layout,sy,pr,en)
    def test_display_1080_requires_expanded_slots_and_separate_control(self):
        sy,pr,en=B.elf_details(synthetic_elf())
        cases=[
            dict(__radar_display_frames_end=0x1c00000,
                 __radar_display_control_start=0x1c00000,
                 __radar_display_control_end=0x1e00000),  # Old SVGA layout.
            dict(__radar_display_control_start=0x1c00000),  # Control overlaps frame1.
            dict(__radar_display_control_end=0x2500000),  # Not a dedicated 2MiB block.
        ]
        for changed in cases:
            with self.subTest(changed=changed),self.assertRaises(B.BuildError):
                B.validate_layout(dict(sy,**changed),pr,en)
    def test_dsp_workspace_and_normal_image_bounds(self):
        sy,pr,en=B.elf_details(synthetic_elf())
        for changed in [dict(__radar_dsp_workspace_start=0x2400000),
                        dict(__radar_dsp_workspace_end=0x3500000),
                        dict(__radar_normal_end=0x210000),dict(radar_dsp_submit=0x2600000)]:
            with self.subTest(changed=changed),self.assertRaises(B.BuildError):
                B.validate_layout(dict(sy,**changed),pr,en)
    def test_driver_copy_hashes(self):
        manifest=json.loads((ROOT/'tests/vendor_amd/ORIGIN_SHA256.json').read_text())
        for name,entry in manifest.items():
            self.assertEqual(hashlib.sha256((ROOT/'tests/vendor_amd'/name).read_bytes()).hexdigest(),entry['sha256'])
    def test_all_production_sources_in_vitis_cmake_and_one_linker(self):
        src=ROOT/'radar_runtime/src';conf=(src/'UserConfig.cmake').read_text()
        sources=list(src.rglob('*.c'))
        self.assertTrue(sources)
        block=re.search(r'set\(USER_COMPILE_SOURCES\s+(.*?)\n\)',conf,re.S)
        self.assertIsNotNone(block)
        listed=re.findall(r'"([^"\n]+\.c)"',block.group(1))
        self.assertCountEqual(listed,[p.relative_to(src).as_posix() for p in sources])
        self.assertEqual(len(list(src.rglob('*.ld'))),1)
        self.assertFalse(list(src.glob('*.c')))
        self.assertIn('lscript_sw4.ld',conf);self.assertNotIn('lscript_sw0.ld',conf)
        self.assertNotIn('linker_gen(', (src/'CMakeLists.txt').read_text())
    def test_cmake_vitis_structure_configures_with_common_api_shim(self):
        # Structural ONLY: no SDK toolchain/BSP link. Detect CMake argument/list
        # errors without suggesting this is a native Vitis build.
        if not shutil.which('cmake'):self.skipTest('CMake not installed')
        with tempfile.TemporaryDirectory() as t:
            p=Path(t);m=p/'modules';m.mkdir()
            (m/'Findcommon.cmake').write_text('''set(common_FOUND TRUE)
function(collect key)
  set_property(GLOBAL APPEND PROPERTY ${key} ${ARGN})
endfunction()
function(collector_list out key)
  get_property(v GLOBAL PROPERTY ${key})
  set(${out} "${v}" PARENT_SCOPE)
endfunction()
function(find_project_type exts out)
  set(${out} "c" PARENT_SCOPE)
endfunction()
function(add_dependency_on_bsp variable)
endfunction()
function(print_elf_size tool app)
endfunction()
''')
            r=subprocess.run(['cmake','-S',str(ROOT/'radar_runtime/src'),'-B',str(p/'native-structure-only'),'-DCMAKE_MODULE_PATH='+str(m),'-DYOCTO=FALSE'],capture_output=True,text=True)
            self.assertEqual(r.returncode,0,r.stdout+r.stderr)

class InstallTests(unittest.TestCase):
    def app(self,p):
        a=p/'radar_runtime';(a/'src').mkdir(parents=True)
        (a/'vitis-comp.json').write_text(json.dumps(dict(cpuInstance='psu_cortexa53_0',domainRealName='standalone_psu_cortexa53_0')))
        (a/'src/main.c').write_text('old main');(a/'src/app.yaml').write_text('LOCAL IDE METADATA')
        (a/'_ide').mkdir();(a/'_ide/launch.json').write_text('LOCAL LAUNCH')
        (a/'build').mkdir();(a/'build/stale.elf').write_text('OLD NOT REUSABLE')
        return a
    def test_dry_run_changes_nothing(self):
        with tempfile.TemporaryDirectory() as t:
            a=self.app(Path(t));plan=I.install(a,ROOT/'radar_runtime/src')
            self.assertEqual(plan['mode'],'DRY_RUN');self.assertEqual((a/'src/main.c').read_text(),'old main')
            self.assertFalse((a/'src/radar_session.c').exists())
    def test_apply_backups_and_preserves_platform_metadata(self):
        with tempfile.TemporaryDirectory() as t:
            a=self.app(Path(t));plan=I.install(a,ROOT/'radar_runtime/src',True,True)
            self.assertEqual((a/'src/app.yaml').read_text(),'LOCAL IDE METADATA')
            self.assertEqual((a/'_ide/launch.json').read_text(),'LOCAL LAUNCH')
            self.assertEqual((Path(plan['backup'])/'src/main.c').read_text(),'old main')
            self.assertTrue((Path(plan['backup'])/'previous_build/stale.elf').exists())
            self.assertEqual((a/'src/capture/radar_session.c').read_bytes(),(ROOT/'radar_runtime/src/capture/radar_session.c').read_bytes())
            self.assertFalse((a/'build').exists())
            self.assertFalse((a/'src/main.c').exists())
    def test_wrong_component_is_rejected(self):
        with tempfile.TemporaryDirectory() as t:
            a=self.app(Path(t));(a/'vitis-comp.json').write_text('{"cpuInstance":"psu_cortexr5_0"}')
            with self.assertRaises(I.InstallError):I.install(a,ROOT/'radar_runtime/src',True)
            self.assertEqual((a/'src/main.c').read_text(),'old main')

    def test_flat_caches_and_old_linker_are_archived_not_left_active(self):
        with tempfile.TemporaryDirectory() as t:
            a=self.app(Path(t));(a/'src/.cache').mkdir();(a/'src/.cache/old.pch').write_text('old')
            (a/'src/lscript_sw0.ld').write_text('OLD LINKER');(a/'src/radar_dma.c').write_text('OLD DMA')
            (a/'src/.clangd').write_text('LOCAL CLANGD')
            plan=I.install(a,ROOT/'radar_runtime/src',True,True)
            self.assertFalse((a/'src/.cache').exists());self.assertFalse((a/'src/radar_dma.c').exists())
            self.assertFalse((a/'src/lscript_sw0.ld').exists())
            self.assertEqual((a/'src/.clangd').read_text(),'LOCAL CLANGD')
            self.assertTrue((Path(plan['backup'])/'src/.cache/old.pch').exists())
    def test_second_install_supports_classified_tree(self):
        with tempfile.TemporaryDirectory() as t:
            a=self.app(Path(t));I.install(a,ROOT/'radar_runtime/src',True,True)
            plan=I.install(a,ROOT/'radar_runtime/src',True,True)
            self.assertTrue((Path(plan['backup'])/'src/app/main.c').exists())
            self.assertTrue((a/'src/config/radar_contract.h').exists())
    def test_local_profile_difference_requires_explicit_authorization(self):
        with tempfile.TemporaryDirectory() as t:
            a=self.app(Path(t));(a/'src/radar_capture_profile_data.h').write_text('USER CHANGED PROFILE')
            with self.assertRaises(I.InstallError):I.install(a,ROOT/'radar_runtime/src',True,True)
            self.assertEqual((a/'src/main.c').read_text(),'old main')
            self.assertTrue((a/'build').exists())
            plan=I.install(a,ROOT/'radar_runtime/src',True,True,True)
            self.assertEqual((Path(plan['backup'])/'src/radar_capture_profile_data.h').read_text(),'USER CHANGED PROFILE')
    def test_symlink_rejected_before_changes(self):
        with tempfile.TemporaryDirectory() as t:
            a=self.app(Path(t));outside=Path(t)/'outside';outside.write_text('DO NOT TOUCH')
            (a/'src/link.c').symlink_to(outside)
            with self.assertRaises(I.InstallError):I.install(a,ROOT/'radar_runtime/src',True,True)
            self.assertEqual(outside.read_text(),'DO NOT TOUCH');self.assertTrue((a/'src/main.c').exists())
    def test_failure_after_source_swap_rolls_back_old_tree_and_build(self):
        from unittest.mock import patch
        with tempfile.TemporaryDirectory() as t:
            a=self.app(Path(t));original=Path.rename
            def fail_build(path, dest):
                if path==a/'build':raise PermissionError('simulated Windows build file lock')
                return original(path,dest)
            with patch.object(Path,'rename',fail_build),self.assertRaises(I.InstallError):
                I.install(a,ROOT/'radar_runtime/src',True,True)
            self.assertEqual((a/'src/main.c').read_text(),'old main')
            self.assertEqual((a/'src/app.yaml').read_text(),'LOCAL IDE METADATA')
            self.assertTrue((a/'build/stale.elf').exists())

if __name__=='__main__':
    unittest.main(verbosity=2)
