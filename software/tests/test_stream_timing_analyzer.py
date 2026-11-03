"""Synthetic log fixtures only; never substitutes for a recorded board run."""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('stream_timing', ROOT/'tools/analyze_stream_timing.py')
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)

def fixture(n=6001, *, skip=(), change=None, first_record=1, page_size=256, step=100000):
    skipped = set(skip)
    rows = []
    for i in range(n):
        k = i+1
        mapped = 1_000_000+i*step
        r = dict(record=k, frame=100+k, epoch=3, seq=k, crc=0x1A2B3C4D,
                 flags=39, stages=319 if k in skipped else 383,
                 raw=i%16, lease=k, task=i%2, tasklease=k,
                 first=mapped*100, last=mapped*100+1_000_000,
                 mapped=mapped, uncertainty=500, observed=mapped+15_000,
                 ready=mapped+20_000, front=mapped+25_000, submitted=mapped+26_000,
                 done=mapped+40_000, published=mapped+55_000,
                 ack=0 if k in skipped else mapped+80_000,
                 cpu=60_000, render=0 if k in skipped else 10_000,
                 queue=0, high=2, revision=200+k, page=0, error=0,
                 bytes=399360, hz=100000000, tstatus=1,
                 ackseq=0 if k in skipped else 1000+k,
                 ackrevision=0 if k in skipped else 200+k,
                 ackframe=0 if k in skipped else 100+k,
                 renderbegin=0 if k in skipped else mapped+56_000,
                 renderend=0 if k in skipped else mapped+66_000,
                 queued=0 if k in skipped else mapped+67_000,
                 coalesced=len([j for j in skipped if j <= k]))
        if change:
            change(r)
        rows.append(r)
    acked = [r for r in rows if r['ack']]
    last_ack = acked[-1]['record'] if acked else -1
    pending = len([k for k in skipped if k >= last_ack])
    actual_skips = len(skipped)-pending
    lines = [f'[STREAM START] frames=0 backend=PL_BASELINE; no retry',
             f'[STREAM STOPPED] verified={n} processed={n} presented={len(acked)} remaining=0 residual=0 error=0 records={n}; evidence retained',
             f'[STREAM STATUS] running=0 limit=0 epoch=3 verified={n} processed={n} presented={len(acked)} ready=0 owners=0 high=2 error=0 records={n} overwritten=0 skipped_display={actual_skips} pending_display={pending} crc_errors=0 hsi_errors=0 dma_errors=0 metadata_errors=0 dp_errors=0 dp_active=1 health_cpu_us=1000000 health_max_us=2000']
    for first in range(first_record-1, n, page_size):
        part = rows[first:first+page_size]
        lines.append(f'@STREAMBEGIN first={first} count={len(part)} records={n} overwritten=0 epoch=3')
        for r in part:
            lines.append('@STREAM '+' '.join(f'{k}={v:x}' if k in mod.HEX_FIELDS else f'{k}={v}' for k,v in r.items()))
        lines.append(f'@STREAMEND next={first+len(part)} records={n} epoch=3')
    return '\n'.join(lines)+'\n'

class StreamTimingAudit(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.full = fixture()

    def codes(self, report):
        return {x['code'] for x in report['issues']}

    def test_complete_synthetic_pass_is_instrumented_only(self):
        r = mod.audit_text(self.full)
        self.assertEqual(r['status'], 'PASS_INSTRUMENTED_GATES', r['issues'][:5])
        self.assertEqual(r['metrics']['observation_span_us'], 600_000_000)
        self.assertEqual(r['metrics']['mapped_first_to_ack_upper_us']['p95'], 80_500)
        self.assertFalse(r['cpu_coverage']['full_cpu_coverage'])
        self.assertEqual(r['cpu_coverage']['full_cpu_80ms_gate'], 'NOT_PROVEN')

    def test_short_run_cannot_pass(self):
        r = mod.audit_text(fixture(64))
        self.assertFalse(r['passed'])
        self.assertFalse(r['gates']['full_untrimmed_observation_600s'])

    def test_missing_record_and_no_cherry_trim(self):
        r = mod.audit_text(fixture(first_record=2))
        self.assertFalse(r['passed'])
        self.assertIn('record_coverage_missing', self.codes(r))
        self.assertEqual(r['definitions']['trimmed_records'], 0)

    def test_overwritten_cannot_pass(self):
        r = mod.audit_text(self.full.replace('overwritten=0', 'overwritten=1'))
        self.assertFalse(r['passed'])
        self.assertIn('records_overwritten', self.codes(r))

    def test_bad_transport_and_mapping(self):
        def corrupt(r):
            if r['record']==100:
                r['bytes']=399359
                r['uncertainty']=1001
        r = mod.audit_text(fixture(change=corrupt))
        self.assertFalse(r['passed'])
        self.assertIn('transport_metadata_invalid', self.codes(r))
        self.assertIn('untrusted_time_mapping', self.codes(r))

    def test_explicit_coalescing_is_not_missing_evidence(self):
        r = mod.audit_text(fixture(skip=range(40,6000,40)))
        self.assertTrue(r['passed'], r['issues'][:5])
        self.assertEqual(r['coverage']['skipped_display'], 149)
        self.assertEqual(r['metrics']['display_interval_us']['maximum'], 200000)
        self.assertGreaterEqual(r['metrics']['unique_display_hz'], 9)

    def test_missing_ack_explanation_and_tail(self):
        r = mod.audit_text(fixture(skip=(100,)).replace('skipped_display=1', 'skipped_display=0'))
        self.assertFalse(r['passed'])
        self.assertIn('missing_ack_accounting', self.codes(r))
        r = mod.audit_text(fixture(skip=(6001,)))
        self.assertFalse(r['passed'])
        self.assertIn('final_pending_display', self.codes(r))

    def test_first_long_gap_is_not_trimmed(self):
        def long_initial_interval(r):
            if r['record']>=2:
                for key in ('mapped','observed','ready','front','submitted','done','published','ack','renderbegin','renderend','queued'):
                    r[key]+=300000
                r['first']+=30_000_000
                r['last']+=30_000_000
        r = mod.audit_text(fixture(change=long_initial_interval))
        self.assertFalse(r['passed'])
        self.assertEqual(r['metrics']['display_interval_us']['maximum'], 400000)
        self.assertFalse(r['gates']['display_interval_max_le_250ms'])

    def test_duplicate_ack_and_identity(self):
        def wrong_ack(r):
            if r['record']==200:
                r['ackframe']+=1
                r['ackseq']-=1
        r = mod.audit_text(fixture(change=wrong_ack))
        self.assertFalse(r['passed'])
        self.assertIn('ack_identity_mismatch', self.codes(r))
        self.assertIn('duplicate_ack_sequence', self.codes(r))

    def test_duplicate_export_is_not_new_frames_and_conflict_fails(self):
        lines=self.full.splitlines()
        first=next(i for i,line in enumerate(lines) if line.startswith('@STREAMBEGIN'))
        last=next(i for i,line in enumerate(lines) if line.startswith('@STREAMEND'))
        page='\n'.join(lines[first:last+1])
        r=mod.audit_text(self.full+page+'\n')
        self.assertTrue(r['passed'],r['issues'][:5])
        self.assertEqual(r['coverage']['identical_duplicate_exports'],256)
        self.assertEqual(r['coverage']['presented'],6001)
        r=mod.audit_text(self.full+page.replace('crc=1a2b3c4d','crc=1a2b3c4e',1)+'\n')
        self.assertFalse(r['passed']);self.assertIn('conflicting_duplicate_record',self.codes(r))

    def test_missing_cpu_health_fields_are_incomplete(self):
        r = mod.audit_text(self.full.replace(' health_cpu_us=1000000', ''))
        self.assertFalse(r['passed'])
        self.assertIn('status_missing_fields', self.codes(r))

    def test_explicit_runtime_error_not_hidden_by_zero_final(self):
        r = mod.audit_text(self.full+'[STREAM ERROR] rc=-603 acquired=6001 processed=6001; controlled stop\n')
        self.assertFalse(r['passed'])
        self.assertIn('runtime_error_line', self.codes(r))

    def test_multiple_run_selection_refused(self):
        r = mod.audit_text('[STREAM START] frames=16 backend=PL_BASELINE\n'+self.full)
        self.assertFalse(r['passed'])
        self.assertIn('one_complete_run_required', self.codes(r))

    def test_bad_cpu_and_missing_envelope(self):
        def cpu(r):
            if r['record']==1:
                r['cpu']=80001
        r = mod.audit_text(fixture(change=cpu))
        self.assertFalse(r['gates']['recorded_kernel_cpu_max_le_80ms'])
        no_end='\n'.join(line for line in self.full.splitlines() if not line.startswith('@STREAMEND'))
        self.assertFalse(mod.audit_text(no_end)['passed'])

if __name__ == '__main__':
    unittest.main(verbosity=2)
