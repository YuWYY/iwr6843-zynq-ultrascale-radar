"""Model sanity checks. Synthetic service budgets cannot certify hardware."""
import importlib.util,json
from pathlib import Path
root=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('queue_model',root/'tools/model_dsp_queue.py')
model=importlib.util.module_from_spec(spec);spec.loader.exec_module(model)
results=[]
for service in (80000,120000,200000):
    r=model.simulate(service,64);results.append(r)
assert results[0]['accepted']==64 and results[0]['high_water']==1 and results[0]['sustainable']
assert results[1]['accepted']==64 and results[1]['high_water']>1 and not results[1]['sustainable']
assert results[2]['overflow_frame'] and results[2]['high_water']==16 and results[2]['accepted']<64
try:model.simulate(0)
except ValueError:pass
else:raise AssertionError('zero service must not count as throughput evidence')
out=root/'verification/20261101_pl_dsp_01/checks/ps_integration/queue_model_sanity.json'
out.write_text(json.dumps({'status':'MODEL_SELFTEST_PASS_MEASURED_SERVICE_PENDING','cases':results},indent=2))
print('PASS100ms queue model: bounded under80ms, growing under120ms, safe stop under200ms; measured throughput pending')
