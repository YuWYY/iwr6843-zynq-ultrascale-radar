"""Audit 64-job co-simulation trace against independent reference manifests."""
from pathlib import Path
import argparse,hashlib,json,re
p=argparse.ArgumentParser();p.add_argument('run');a=p.parse_args();run=Path(a.run)
manifest=json.loads((run/'reference/reference_manifest.json').read_text());log=(run/'run.log').read_text(errors='replace')
jobs=[json.loads(line) for line in (run/'results/jobs.jsonl').read_text().splitlines()]
cache={}
def digest(file):
    if file not in cache:
        h=0xcbf29ce484222325
        for value in file.read_text().splitlines():h=((h^int(value,16))*0x100000001b3)&((1<<64)-1)
        cache[file]=f'{h:016x}'
    return cache[file]
passed=len(jobs)==64 and 'PASS dsp_batch64' in log and not re.search(r'(?im)^\s*(?:ERROR:|Fatal:)',log)
for variant in manifest['variants']:
    directory=run/'reference'/f"variant{variant['variant']:02d}"
    for name,expected_hash in variant['files'].items():passed&=hashlib.sha256((directory/name).read_bytes()).hexdigest()==expected_hash
details=[]
for index,job in enumerate(jobs):
    reference=manifest['jobs'][index];variant=run/'reference'/f"variant{reference['variant']:02d}";expected=run/'reference'/f'job{index:02d}'
    ok=all(job[k]==v for k,v in {'job':index+1,'variant':reference['variant'],'timestamp_us':reference['timestamp_us'],'epoch':reference['config_epoch'],
          'acquisition':reference['acquisition_id'],'frame':reference['frame_index'],'slots':reference['input_slots'],'slot':reference['input_slot'],
          'flags':reference['flags'],'candidates':reference['candidate_count'],'reads':14721,'writes':11653,'pass':True}.items())
    for key,name in [('range','range'),('cube','cube'),('rd','rd'),('spectrum','spectrum')]:ok&=job[f'{key}_fnv_word64']==digest(variant/f'{name}.hex')
    ok&=job['temporal_fnv_word64']==digest(expected/'temporal.hex')
    for name,expected_hash in reference['files'].items():ok&=hashlib.sha256((expected/name).read_bytes()).hexdigest()==expected_hash
    details.append({**job,'reference_input_sha256':manifest['variants'][reference['variant']]['files']['input.hex'],'pass':bool(ok)})
    passed&=ok
result={'status':'PASS' if passed else 'FAILED','evidence_class':'production_RTL_and_official_XPM_with_AMD_FFT_Cmodel_DPI',
        'native_fft_protocol_or_latency_proof':False,'hardware_access':False,'timestamp_is_synthetic':True,
        'latency_adapter_cycles':{'range_after_last_input':1024,'doppler_after_last_input':256},
        'sequential_jobs':len(jobs),'resets_between_jobs':0,'test_only_layout':True,'input_slots':[8,16],
        'source_manifest':'source_manifest.json','raw_archived_job':64,'raw_definition':manifest['variants'][12]['definition'],
        'candidate_cycle_word':'Compared all candidate words except dynamic cycle word; its digest is recorded but not a numerical-reference field.',
        'jobs':details}
(run/'summary.json').write_text(json.dumps(result,indent=2));print(json.dumps({k:v for k,v in result.items() if k not in ('jobs','raw_definition')},indent=2))
if not passed:raise SystemExit(1)
