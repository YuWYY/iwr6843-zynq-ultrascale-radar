"""Real current-source XSim suite. Each output directory is a fresh attempt.

Full suite emits SIM_PASS.json only after all required simulations and immutable
input/log checks. --tests runs a diagnostic subset and cannot emit a release gate.
"""
from pathlib import Path
import argparse, datetime, hashlib, json, os, re, subprocess

ROOT=Path(__file__).resolve().parents[2]
VIVADO=Path('C:/Xilinx/2025.1/Vivado')
REQUIRED=Path(__file__).with_name('required_tests.json')
definition=json.loads(REQUIRED.read_text())
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--tests',nargs='+')
parser.add_argument('--output',default='outputs/m2_closure_'+datetime.datetime.now().strftime('%Y%m%d_%H%M%S')+'/sim')
args=parser.parse_args()
tests=args.tests or definition['tests']
out=ROOT/args.output
out.mkdir(parents=True,exist_ok=False)
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
sources=sorted(p for p in (ROOT/'rtl').rglob('*') if p.suffix in {'.v','.sv','.vh'})
benches=[ROOT/'sim'/f'{t}.sv' for t in tests]
fifo=ROOT/'Radar_XCZU2CG_M2.gen/sources_1/bd/radar_system/ip/radar_system_axis_data_fifo_0_0/sim/radar_system_axis_data_fifo_0_0.v'
fifo_xci=ROOT/'Radar_XCZU2CG_M2.srcs/sources_1/bd/radar_system/ip/radar_system_axis_data_fifo_0_0/radar_system_axis_data_fifo_0_0.xci'
inputs=sources+benches+[REQUIRED,Path(__file__).resolve()]
if 'tb_capture_fifo' in tests: inputs += [fifo,fifo_xci]
input_hashes={p.relative_to(ROOT).as_posix():sha(p) for p in inputs}
(out/'input_hashes.json').write_text(json.dumps(input_hashes,indent=2),encoding='utf-8')
(out/'run_all.tcl').write_text('run all\nquit\n',encoding='utf-8')
state={'state':'RUNNING','tests':{},'required_suite':not bool(args.tests)}
env=os.environ.copy()
env['XILINX_VIVADO']=str(VIVADO)
env['PATH']=str(VIVADO/'bin')+os.pathsep+env['PATH']
def save():
    temporary=out/'STATUS.json.tmp'
    temporary.write_text(json.dumps(state,indent=2),encoding='utf-8')
    temporary.replace(out/'STATUS.json')
def run(tool,params,name,timeout=600):
    command=[str(VIVADO/'bin'/(tool+'.bat')),*map(str,params)]
    with (out/(name+'.log')).open('w',encoding='utf-8') as log:
        log.write(subprocess.list2cmdline(command)+'\n');log.flush()
        done=subprocess.run(command,cwd=out,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=timeout)
    text=(out/(name+'.log')).read_text(errors='replace')
    if done.returncode or re.search(r'(?im)^\s*(?:ERROR:|FATAL|Fatal:)',text):
        raise RuntimeError(f'{name} failed rc={done.returncode}: {out/(name+".log")}')
    return text
save()
try:
    run('xvlog',[p for p in sources if p.suffix=='.v'],'compile_verilog')
    sv=[p for p in sources if p.suffix=='.sv']
    if sv:run('xvlog',['--sv',*sv],'compile_sv_production')
    run('xvlog',['--sv',*benches],'compile_testbenches')
    run('xvlog',[VIVADO/'data/verilog/src/glbl.v'],'compile_glbl')
    if 'tb_capture_fifo' in tests:run('xvlog',[fifo],'compile_fifo')
    for tb in tests:
        libraries=['unisims_ver','unimacro_ver','secureip','xpm']
        if tb=='tb_capture_fifo':libraries+=['axis_data_fifo_v2_0_17','axis_infrastructure_v1_1_1','fifo_generator_v13_2_13']
        params=[tb,'glbl']
        for lib in libraries:params+=['-L',lib]
        snapshot=tb+'_snapshot'
        run('xelab',params+['-s',snapshot],'elaborate_'+tb)
        for phase in [None,*definition['phase_tests'].get(tb,[])]:
            name=tb+('_'+phase if phase else '')
            params=[snapshot,'-tclbatch','run_all.tcl']
            if phase:params+=['-testplusarg',phase]
            text=run('xsim',params,'run_'+name)
            if not re.search(r'PASS\s+'+re.escape(tb)+r'\b',text):raise RuntimeError(name+' missing actual PASS marker')
            state['tests'][name]='PASS';save();print('PASS',name,flush=True)
    for path,digest in input_hashes.items():
        if sha(ROOT/path)!=digest:raise RuntimeError('Input changed during simulation: '+path)
    state['state']='PASS';save()
    if not args.tests:
        suite=out.relative_to(ROOT).as_posix()
        gate={'status':'PASS','suite':suite,'tests':sorted(state['tests']),
              'input_hashes':input_hashes,
              'log_hashes':{p.relative_to(ROOT).as_posix():sha(p) for p in out.glob('*.log')}}
        (out/'SIM_PASS.json').write_text(json.dumps(gate,indent=2),encoding='utf-8')
        print('SIMULATION_GATE='+str(out/'SIM_PASS.json'),flush=True)
except Exception as exc:
    state.update(state='FAIL',error=str(exc));save();raise
