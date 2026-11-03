#!/usr/bin/env python3
"""Read-only DP observation audit. Never overrides the strict timing audit.

Equal HPD-ISR observation counts are not a zero-hardware-event proof.
Diagnostic counters precede register reads; this snapshot is not atomic.
"""
from __future__ import annotations
import argparse
from datetime import datetime
import hashlib
import json
from pathlib import Path
import re
from typing import Any

SCHEMA = 'M2_DP_OBSERVED_SCOPE_V1'
REGS = set('0084 0110 0180 0184 0188 018c 0190 0194 0198 019c 01a0 01a4 01a8 01ac 01b0 01b4 01b8 01bc 01c4 01c8 01cc 0280 a00c a014 a018 a01c b00c b01c b070 b120 b124'.split())
LIMITS = [
    'Not a proof of zero physical DP faults or an exact hardware-event count.',
    'TX_fifo_observations counts nonzero reads in HPD ISR only; no dedicated FIFO interrupt coverage.',
    'Diagnostic counters precede register reads: an ISR between those reads can escape one non-atomic snapshot.',
    'Only recorded commands/application observations are covered; external debugger/MMIO reads are not observable.',
    'No general PS register read-clear semantics inferred from a PL DisplayPort product guide.',
    'Original strict timing FAIL and CPU NOT_PROVEN remain unchanged; no unconditional acceptance.',
]

def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()

def normal(text: str) -> str:
    return '\n'.join(x.strip() for x in text.replace('\r','').splitlines() if x.strip())

def diagnostic(text: str) -> dict[str, Any]:
    text = normal(text)
    def one(pattern):
        hits = re.findall(pattern,text,re.M)
        if len(hits)!=1:
            raise ValueError('expected_one:'+pattern)
        return hits[0]
    head = one(r'^\[DISPLAY DIAG\] HPD=(\d+) lanes=(\d+) link_rate=([0-9a-fA-F]+) pixel=(\d+) errors=([0-9a-fA-F]+)/([0-9a-fA-F]+)$')
    latch = one(r'^\[DISPLAY DIAG\] DP_irq_latched=([0-9a-fA-F]+) TX_fifo_latched=([0-9a-fA-F]+)$')
    obs = one(r'^\[DISPLAY DIAG\] TX_fifo_observations=(\d+) \(ISR observations, includes startup\)$')
    one(r'^\[DISPLAY DIAG END\]$')
    rows = re.findall(r'^\[DISPLAY REG\] ([0-9a-fA-F]{4})=([0-9a-fA-F]{8})$',text,re.M)
    regs = {k.lower():int(v,16) for k,v in rows}
    if len(rows)!=len(regs) or set(regs)!=REGS:
        raise ValueError('incomplete_or_duplicate_registers')
    dpcd = re.findall(r'^\[DISPLAY DPCD\] ([0-9a-f]{3}) rc=(\d+) data=([0-9a-f]{16})$',text,re.M)
    if len(dpcd)!=3 or {x[0] for x in dpcd}!={'100','200','210'}:
        raise ValueError('incomplete_dpcd')
    edid = re.findall(r'^\[DISPLAY EDID\] ([0-9a-f]{3}) ([0-9a-f]{64})$',text,re.M)
    if len(edid) not in (4,8) or [int(x[0],16) for x in edid]!=list(range(0,len(edid)*32,32)):
        raise ValueError('incomplete_edid')
    return dict(hpd=int(head[0]),lanes=int(head[1]),rate=int(head[2],16),pixel=int(head[3]),
                isr=int(head[4],16),eisr=int(head[5],16),irq=int(latch[0],16),
                fifo_latched=int(latch[1],16),observations=int(obs),registers=regs,
                dpcd={a:dict(rc=int(rc),data=d) for a,rc,d in dpcd},edid=''.join(d for _,d in edid))

def observations(before: str,after: str,scope: str,commands: list[str],final_mask: int) -> dict[str,Any]:
    """Pure evidence checks; recorder provenance is added by audit_run."""
    issues=[]
    def issue(code,detail=None,fail=False):
        issues.append(dict(code=code,kind='FAIL' if fail else 'INCOMPLETE',detail=detail))
    try:
        b,a=diagnostic(before),diagnostic(after)
    except ValueError as e:
        return dict(status='DP_SCOPE_INCOMPLETE',issues=[dict(code='diagnostic_parse',detail=str(e),kind='INCOMPLETE')])
    for name,d in [('before',b),('after',a)]:
        if d['hpd']!=1 or d['registers']['0084']!=1: issue(name+'_inactive_link',fail=True)
        if d['isr'] or d['eisr']: issue(name+'_dpdma_error',[d['isr'],d['eisr']],fail=True)
        if d['registers']['0110']: issue(name+'_fifo_register_nonzero',d['registers']['0110'],fail=name=='after')
        if any(x['rc']!=0 for x in d['dpcd'].values()): issue(name+'_dpcd_read_failed',fail=True)
        if d['fifo_latched'] not in (0,1) or not 0<=d['observations']<0xFFFFFFFF:
            issue(name+'_unknown_latch_or_counter',fail=True)
        if bool(d['fifo_latched'])!=bool(d['observations']): issue(name+'_latch_counter_inconsistent')
    for field in ('fifo_latched','observations','irq','hpd','lanes','rate','pixel','edid'):
        if b[field]!=a[field]:
            issue(field+'_changed',[b[field],a[field]],fail=field in ('fifo_latched','observations','irq'))
    for reg in sorted(REGS-{'0110'}):
        if b['registers'][reg]!=a['registers'][reg]: issue('register_changed',reg)
    if b['dpcd']!=a['dpcd']: issue('sink_state_changed')
    expected=8 if a['fifo_latched'] else 0
    if final_mask!=expected: issue('final_fault_mask_not_explained_by_unchanged_fifo_latch',final_mask,fail=True)
    if commands.count('display diag')!=2 or commands[:1]!=['display diag'] or commands[-1:]!=['display diag']:
        issue('diagnostic_command_coverage')
    if commands.count('scope live')!=1: issue('capture_start_count')
    allowed=re.compile(r'^(?:display diag|display view (?:overview|azimuth|top|space)|scope (?:live|stop|status|scene))$')
    for cmd in commands:
        if not allowed.fullmatch(cmd): issue('unreviewed_command_in_scope',cmd)
    if scope.count('[DISPLAY DIAG END]')!=2 or len(re.findall(r'\[DISPLAY REG\] 0110=',scope))!=2:
        issue('unaccounted_or_missing_diagnostic_read')
    starts=[x.start() for x in re.finditer(r'\[STREAM START\]',scope)]
    stops=[x.start() for x in re.finditer(r'\[STREAM STOPPED\]',scope)]
    ends=[x.start() for x in re.finditer(r'\[DISPLAY DIAG END\]',scope)]
    heads=[x.start() for x in re.finditer(r'\[DISPLAY DIAG\] HPD=',scope)]
    if not (len(starts)==len(stops)==1 and len(ends)==len(heads)==2 and ends[0]<starts[0]<stops[0]<heads[1]):
        issue('run_not_bracketed_by_complete_diagnostics')
    faults=re.findall(r'^.*(?:F20 RADAR RUNTIME|\[FAULT\]|\[FAIL\]|\[STREAM (?:ERROR|FAIL)\]|DPDMA error|link changed|acknowledgement timeout|training|init failed).*$',scope,re.M|re.I)
    if faults: issue('reset_link_or_error_text',faults,fail=True)
    status='DP_SCOPE_FAIL' if any(i['kind']=='FAIL' for i in issues) else 'DP_SCOPE_INCOMPLETE' if issues else 'NO_NEW_DP_ERROR_OBSERVED_IN_SCOPE'
    return dict(status=status,issues=issues,before=b,after=a,observation_counter_delta=a['observations']-b['observations'],
                final_lifetime_dp_error_mask=final_mask,exact_zero_event_proof=False)

def audit_run(stage: Path,session: Path,timing: Path,launch_identity: Path,display_source: Path) -> dict[str,Any]:
    inputs=[];issues=[]
    def read(path):
        data=path.read_bytes();inputs.append(dict(path=str(path.resolve()),bytes=len(data),sha256=digest(data)));return data
    def problem(code,detail=None):
        issues.append(dict(code=code,kind='INCOMPLETE',detail=detail))
    result=json.loads(read(stage/'result.json'));original=json.loads(read(timing))
    before=read(stage/'display_diag_before.txt').decode('utf-8-sig');after=read(stage/'display_diag.txt').decode('utf-8-sig')
    identity=json.loads(read(launch_identity));read(display_source)
    for entry in identity.get('inputs',[]):
        if digest(read(Path(entry['path'])))!=entry['sha256']: problem('sealed_launch_input_changed',entry['name'])
    for entry in original.get('input_files',[]):
        if digest(read(Path(entry['path'])))!=entry['sha256']: problem('timing_input_changed',entry['path'])
    if result.get('status')!='COMPLETE_OBSERVATION_PENDING_AUDIT': problem('run_not_completed',result.get('status'))
    events=[json.loads(x) for x in read(session/'events.jsonl').splitlines() if x.strip()]
    raw=read(session/'received.bin')
    replies=[]
    for path in sorted((session/'replies').glob('*.json')):
        obj=json.loads(path.read_text(encoding='utf-8-sig'))
        if obj.get('command')=='display diag' and obj.get('rx_offset',-1)>=result['serial_offset'] and datetime.fromisoformat(obj['finished']).timestamp()<=result['finished']:
            replies.append((path,obj))
    if len(replies)!=2: raise ValueError('need_exactly_two_diagnostic_replies_in_stage:'+str(len(replies)))
    (bp,br),(ap,ar)=replies;lo,hi=br['rx_offset'],ar['rx_end'];bt,at=br['started'],ar['finished']
    selected=[e for e in events if bt<=e['time']<=at]
    commands=[e['command'] for e in selected if e['kind']=='TX']
    for path,obj in replies:
        read(path)
        if obj.get('status')!='PASS' or obj['received'].encode('ascii')!=raw[obj['rx_offset']:obj['rx_end']]:
            problem('reply_not_matching_raw',str(path))
    if normal(before)!=normal(br['received']) or normal(after)!=normal(ar['received']): problem('saved_diagnostic_not_matching_reply')
    cursor=lo
    for e in events:
        if e['kind']!='RX': continue
        first,last=e['offset'],e['offset']+e['bytes']
        if last<=lo or first>=hi: continue
        if max(lo,first)!=cursor: problem('rx_byte_coverage_gap_or_overlap',[cursor,first,last])
        cursor=min(hi,last)
    if cursor!=hi: problem('rx_byte_coverage_incomplete',[cursor,hi])
    for e in selected:
        if e['kind']=='TX':
            path=session/'replies'/e['request'];obj=json.loads(read(path))
            if obj.get('status')!='PASS' or obj.get('command')!=e['command']: problem('command_reply_not_pass',e['request'])
            if obj.get('received','').encode('ascii')!=raw[obj['rx_offset']:obj['rx_end']]: problem('command_reply_bytes_changed',e['request'])
        elif e['kind'] not in ('RX','REQUEST_END'): problem('unexpected_recorder_event',e)
    scope_bytes=raw[lo:hi]
    report=observations(before,after,scope_bytes.decode('ascii','replace'),commands,original.get('final_status',{}).get('dp_errors',-1))
    report['issues'].extend(issues)
    if issues and report['status']=='NO_NEW_DP_ERROR_OBSERVED_IN_SCOPE': report['status']='DP_SCOPE_INCOMPLETE'
    report.update(schema=SCHEMA,scope='Complete recorded before-diagnostic through after-diagnostic; no trimming',
                  input_files=inputs,raw_scope=dict(path=str((session/'received.bin').resolve()),offset=lo,end=hi,bytes=hi-lo,sha256=digest(scope_bytes)),
                  commands=commands,limits=LIMITS,strict_timing_status=original.get('status'),strict_timing_issues=original.get('issues'),
                  original_report_unchanged=True,instrumented_timing_gates=original.get('gates'),full_cpu_budget='NOT_PROVEN',overall_unconditional_acceptance=False)
    return report

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('stage','session','timing-audit','launch-identity','display-source','output'):
        p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args()
    if a.output.exists(): p.error('Output must be a new path; historical reports are immutable')
    try: report=audit_run(a.stage,a.session,a.timing_audit,a.launch_identity,a.display_source)
    except (OSError,ValueError,KeyError,TypeError) as e:
        report=dict(schema=SCHEMA,status='DP_SCOPE_INCOMPLETE',issues=[dict(code='input_or_provenance_error',detail=str(e))],limits=LIMITS,overall_unconditional_acceptance=False)
    a.output.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(dict(status=report['status'],issues=report['issues'],output=str(a.output)),indent=2))
    return 0 if report['status']=='NO_NEW_DP_ERROR_OBSERVED_IN_SCOPE' else 1

if __name__=='__main__':
    raise SystemExit(main())
