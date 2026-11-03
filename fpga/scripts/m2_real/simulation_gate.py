"""Create/check a current-source gate from a completed, actual XSim suite."""
from pathlib import Path
import argparse, hashlib, json, re
root=Path(__file__).resolve().parents[2]
gate=root/'delivery/20261012_171114/SIM_PASS.json'
required={'tb_decoder','tb_packetizer','tb_io_guard','tb_cdc','tb_control','tb_phy_unisim','tb_capture_fifo','tb_delay_transaction','tb_delay_sequencer','tb_early_clock_loss'}
required.update('tb_delay_transaction_'+p for p in ['PHASE000','PHASE025','PHASE125','PHASE225'])
ap=argparse.ArgumentParser();ap.add_argument('--suite');ap.add_argument('--check',action='store_true');args=ap.parse_args()
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
if args.check:
    if not gate.exists():gate=root.parent/'evidence/delivery/SIM_PASS.json'
    result=json.loads(gate.read_text()); assert result['status']=='PASS'
    suite_rel=result['suite']
    suite=root/suite_rel
    if not suite.exists():suite=root.parent/'evidence'/suite_rel
else:
    assert args.suite,'Provide --suite <relative suite path>'
    suite_rel=args.suite
    suite=root/args.suite
state=json.loads((suite/'STATUS.json').read_text())
assert state['state']=='PASS' and required.issubset(state['tests'])
assert all(state['tests'][t]=='PASS' for t in required)
hashes=json.loads((suite/'input_hashes.json').read_text())
for name, expected in hashes.items(): assert sha(root/name)==expected, f'Changed simulation input: {name}'
assert {p.relative_to(root).as_posix() for p in (root/'rtl').rglob('*.v')}.issubset(hashes)
for tb in required:
    log=(suite/f'run_{tb}.log').read_text(errors='replace')
    marker=tb.split('_PHASE')[0]
    assert re.search(r'PASS\s+'+re.escape(marker)+r'\b',log), tb
    assert not re.search(r'(?im)^\s*(?:ERROR:|FATAL|Fatal:)',log),tb
result={'status':'PASS','suite':Path(suite_rel).as_posix(),'tests':sorted(required),'input_hashes':hashes,'log_hashes':{(Path(suite_rel)/p.name).as_posix():sha(p) for p in suite.glob('*.log')}}
if args.check:
    recorded=json.loads(gate.read_text())
    assert recorded==result, 'Gate evidence changed; rerun and reissue explicitly'
else:
    gate.parent.mkdir(parents=True,exist_ok=True)
    gate.write_text(json.dumps(result,indent=2),encoding='utf-8')
print('CURRENT_SOURCE_SIMULATION_GATE=PASS')
