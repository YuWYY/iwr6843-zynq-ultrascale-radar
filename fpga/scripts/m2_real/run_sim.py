"""Run current production Verilog with actual Vivado 2025.1 XSim libraries."""
from pathlib import Path
import argparse, subprocess, os, json, hashlib, re, datetime
root=Path(__file__).resolve().parents[2]
vivado=Path('C:/Xilinx/2025.1/Vivado')
ap=argparse.ArgumentParser()
ap.add_argument('--tests',nargs='+',default=['tb_decoder','tb_packetizer','tb_io_guard','tb_cdc','tb_control','tb_phy_unisim','tb_capture_fifo','tb_delay_transaction','tb_delay_sequencer','tb_early_clock_loss'])
ap.add_argument('--output',default='outputs/m2_real_20261012_171114/xsim_'+datetime.datetime.now().strftime('%H%M%S'))
args=ap.parse_args()
out=root/args.output; out.mkdir(parents=True,exist_ok=False)
env=os.environ.copy();env['XILINX_VIVADO']=str(vivado);env['PATH']=str(vivado/'bin')+os.pathsep+env['PATH']
state={'state':'RUNNING','tests':{}}
inputs=list((root/'rtl').rglob('*.v'))+[root/'sim'/f'{t}.sv' for t in args.tests]
if 'tb_capture_fifo' in args.tests:
    inputs += [root/'Radar_XCZU2CG_M2.gen/sources_1/bd/radar_system/ip/radar_system_axis_data_fifo_0_0/sim/radar_system_axis_data_fifo_0_0.v',
               root/'Radar_XCZU2CG_M2.srcs/sources_1/bd/radar_system/ip/radar_system_axis_data_fifo_0_0/radar_system_axis_data_fifo_0_0.xci']
manifest={p.relative_to(root).as_posix():hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
(out/'input_hashes.json').write_text(json.dumps(manifest,indent=2))
(out/'run_all.tcl').write_text('run all\nquit\n')
def save(): (out/'STATUS.json').write_text(json.dumps(state,indent=2))
def run(tool,params,name,timeout=600):
    command=[str(vivado/'bin'/(tool+'.bat')),*map(str,params)]
    with (out/(name+'.log')).open('w',encoding='utf-8') as log:
        log.write(subprocess.list2cmdline(command)+'\n');log.flush()
        done=subprocess.run(command,cwd=out,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=timeout)
    text=(out/(name+'.log')).read_text(errors='replace')
    if done.returncode or re.search(r'(?im)^\s*(?:ERROR:|FATAL|Fatal:)',text):
        raise RuntimeError(f'{name} failed with rc={done.returncode}; see {out/(name+".log")}')
    return text
save()
try:
    run('xvlog',sorted((root/'rtl').rglob('*.v')),'compile_verilog')
    run('xvlog',['--sv',*[root/'sim'/f'{t}.sv' for t in args.tests]],'compile_testbenches')
    run('xvlog',[vivado/'data/verilog/src/glbl.v'],'compile_glbl')
    if 'tb_capture_fifo' in args.tests:
        fifo=root/'Radar_XCZU2CG_M2.gen/sources_1/bd/radar_system/ip/radar_system_axis_data_fifo_0_0/sim/radar_system_axis_data_fifo_0_0.v'
        run('xvlog',[fifo],'compile_fifo')
    for tb in args.tests:
        params=[tb,'glbl','-L','unisims_ver','-L','unimacro_ver','-L','secureip']
        if tb=='tb_capture_fifo':
            params+=['-L','axis_data_fifo_v2_0_17','-L','axis_infrastructure_v1_1_1','-L','fifo_generator_v13_2_13','-L','xpm']
        run('xelab',params+['-s',tb+'_snapshot'],'elaborate_'+tb)
        text=run('xsim',[tb+'_snapshot','-tclbatch','run_all.tcl'],'run_'+tb)
        if not re.search(r'PASS\s+'+re.escape(tb)+r'\b',text): raise RuntimeError(tb+' missing PASS marker')
        state['tests'][tb]='PASS';save();print('PASS',tb,flush=True)
        if tb=='tb_delay_transaction':
            for phase in ['PHASE000','PHASE025','PHASE125','PHASE225']:
                text=run('xsim',[tb+'_snapshot','-testplusarg',phase,'-tclbatch','run_all.tcl'],'run_'+tb+'_'+phase)
                if not re.search(r'PASS\s+'+re.escape(tb)+r'\b',text): raise RuntimeError(tb+' '+phase+' missing PASS marker')
                state['tests'][tb+'_'+phase]='PASS';save();print('PASS',tb,phase,flush=True)
    state['state']='PASS';save()
except Exception as exc:
    state.update(state='FAIL',error=str(exc));save();raise
