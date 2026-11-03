"""Synthetic evidence only. Never substitutes for hardware event coverage."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('dp_scope',ROOT/'tools/audit_stream_dp_scope.py')
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)

def diag(obs=1,reg=0,latch=1,isr=0):
    rows=['[DISPLAY DIAG] HPD=1 lanes=2 link_rate=0a pixel=148500000 errors=%08x/00000000'%isr,
          '[DISPLAY DIAG] DP_irq_latched=0000c002 TX_fifo_latched=%08x'%latch,
          '[DISPLAY DIAG] TX_fifo_observations=%d (ISR observations, includes startup)'%obs]
    for addr in sorted(m.REGS):
        rows.append('[DISPLAY REG] %s=%08x'%(addr,1 if addr=='0084' else reg if addr=='0110' else 0))
    rows += ['[DISPLAY DPCD] %s rc=0 data=0000000000000000'%x for x in ('100','200','210')]
    rows += ['[DISPLAY EDID] %03x %s'%(i,'0'*64) for i in range(0,128,32)]
    return '\n'.join(rows+['[DISPLAY DIAG END]'])+'\n'

def check(before=None,after=None,commands=None,extra='',mask=8):
    b=before or diag();a=after or diag()
    c=commands or ['display diag','scope live','scope status','display diag']
    scope=b+'[STREAM START]\n'+extra+'[STREAM STOPPED]\n'+a
    return m.observations(b,a,scope,c,mask)

class TestObservation(unittest.TestCase):
    def test_existing_latch_is_only_qualified_observation(self):
        r=check();self.assertEqual(r['status'],'NO_NEW_DP_ERROR_OBSERVED_IN_SCOPE')
        self.assertFalse(r['exact_zero_event_proof']);self.assertEqual(r['final_lifetime_dp_error_mask'],8)
    def test_same_sticky_bit_new_count_is_failure(self):
        self.assertEqual(check(after=diag(obs=2))['status'],'DP_SCOPE_FAIL')
    def test_new_nonzero_register_is_failure(self):
        self.assertEqual(check(after=diag(reg=1))['status'],'DP_SCOPE_FAIL')
    def test_old_nonzero_before_remains_unresolved(self):
        self.assertEqual(check(before=diag(reg=1))['status'],'DP_SCOPE_INCOMPLETE')
    def test_reset_counter_is_not_no_new_errors(self):
        self.assertEqual(check(after=diag(obs=0,latch=0),mask=0)['status'],'DP_SCOPE_FAIL')
    def test_other_fault_never_masked(self):
        for mask in (1,2,4,9,10,12,16):
            self.assertEqual(check(mask=mask)['status'],'DP_SCOPE_FAIL')
        self.assertEqual(check(after=diag(isr=1))['status'],'DP_SCOPE_FAIL')
    def test_hidden_midrun_register_read_is_not_ignored(self):
        self.assertEqual(check(extra='[DISPLAY REG] 0110=00000001\n')['status'],'DP_SCOPE_INCOMPLETE')
    def test_restart_command_or_banner_is_not_accepted(self):
        self.assertEqual(check(commands=['display diag','scope live','display init','display diag'])['status'],'DP_SCOPE_INCOMPLETE')
        self.assertEqual(check(extra='========== F20 RADAR RUNTIME ==========\n')['status'],'DP_SCOPE_FAIL')
    def test_incomplete_diagnostics_refused(self):
        self.assertEqual(check(after=diag().replace('[DISPLAY DIAG END]',''))['status'],'DP_SCOPE_INCOMPLETE')
    def test_no_data_is_not_complete_run(self):
        r=m.observations(diag(),diag(),diag()+diag(),['display diag','scope live','display diag'],8)
        self.assertEqual(r['status'],'DP_SCOPE_INCOMPLETE')
    def test_clean_lifetime_also_supported(self):
        self.assertEqual(check(before=diag(0,0,0),after=diag(0,0,0),mask=0)['status'],'NO_NEW_DP_ERROR_OBSERVED_IN_SCOPE')

class TestRecorder(unittest.TestCase):
    def fixture(self,root):
        stage=root/'stage';session=root/'session';stage.mkdir();session.mkdir();(session/'replies').mkdir()
        raw=b'';events=[];commands=[('display diag',diag()),('scope live','[STREAM START]\n'),
                                 ('scope status','[STREAM STOPPED]\n'),('display diag',diag())]
        for i,(cmd,text) in enumerate(commands):
            name=str(i)+'.json';offset=len(raw);raw+=text.encode('ascii')
            start='2026-11-02T00:00:%02d+00:00'%(i*3);finish='2026-11-02T00:00:%02d+00:00'%(i*3+2)
            reply=dict(command=cmd,status='PASS',received=text,rx_offset=offset,rx_end=len(raw),started=start,finished=finish)
            (session/'replies'/name).write_text(json.dumps(reply))
            events.extend([dict(kind='TX',command=cmd,request=name,time=start),
                           dict(kind='RX',offset=offset,bytes=len(text),time=start),
                           dict(kind='REQUEST_END',request=name,status='PASS',time=finish)])
        (session/'received.bin').write_bytes(raw)
        (session/'events.jsonl').write_text('\n'.join(json.dumps(e) for e in events))
        (stage/'result.json').write_text(json.dumps(dict(status='COMPLETE_OBSERVATION_PENDING_AUDIT',serial_offset=0,finished=1793577700)))
        (stage/'display_diag_before.txt').write_text(diag());(stage/'display_diag.txt').write_text(diag())
        timing=stage/'timing.json';timing.write_text(json.dumps(dict(status='FAIL',final_status=dict(dp_errors=8),issues=[dict(code='final_dp_errors')],gates=dict(observation_span_ge_600s=False))))
        identity=root/'identity.json';identity.write_text('{"inputs":[]}')
        source=root/'source.c';source.write_text('/* synthetic */')
        return (stage,session,timing,identity,source)
    def test_complete_provenance_preserves_original_failure(self):
        with tempfile.TemporaryDirectory() as d:
            args=self.fixture(Path(d));original=args[2].read_bytes()
            r=m.audit_run(*args)
            self.assertEqual(r['status'],'NO_NEW_DP_ERROR_OBSERVED_IN_SCOPE')
            self.assertEqual(r['strict_timing_status'],'FAIL')
            self.assertFalse(r['overall_unconditional_acceptance'])
            self.assertEqual(args[2].read_bytes(),original)
            self.assertTrue(all(len(x['sha256'])==64 for x in r['input_files']))
    def test_missing_raw_bytes_is_incomplete(self):
        with tempfile.TemporaryDirectory() as d:
            args=self.fixture(Path(d));path=args[1]/'events.jsonl'
            lines=path.read_text().splitlines();del lines[4];path.write_text('\n'.join(lines))
            self.assertEqual(m.audit_run(*args)['status'],'DP_SCOPE_INCOMPLETE')
    def test_tampered_saved_diag_is_incomplete(self):
        with tempfile.TemporaryDirectory() as d:
            args=self.fixture(Path(d));(args[0]/'display_diag.txt').write_text(diag().replace('148500000','148500001'))
            self.assertEqual(m.audit_run(*args)['status'],'DP_SCOPE_INCOMPLETE')

if __name__=='__main__':
    unittest.main()
