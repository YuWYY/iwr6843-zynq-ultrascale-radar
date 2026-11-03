"""Reproduce offline PL DSP evidence. No project write, board or serial access.

--mode cosim: production engine + official XPM + AMD FFT DLL through DPI.
--mode native: production engine + official XPM + original native FFT HDL.
The modes are separate evidence classes; cosim is not FFT protocol/timing proof.
"""
from pathlib import Path
import argparse,hashlib,json,os,re,shutil,subprocess,sys,time,zipfile

parser=argparse.ArgumentParser()
parser.add_argument('--output',required=True)
parser.add_argument('--temporal-model',required=True,help='directory containing independently maintained temporal_model.py')
parser.add_argument('--vivado',default='C:/Xilinx/2025.1/Vivado')
parser.add_argument('--mode',choices=('cosim','native','all'),default='all')
args=parser.parse_args()
here=Path(__file__).resolve().parent;project=here.parents[1]
out=Path(args.output).resolve();out.mkdir(parents=True,exist_ok=False)
vivado=Path(args.vivado).resolve();fixture=out/'fixture';vendor=out/'vendor_cmodel'
summary={'status':'RUNNING','mode':args.mode,'evidence':[],'started':time.time()}
sources=list((project/'rtl/dsp').glob('*.sv'))+list(here.glob('*'))
def hashes():
    return {str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in sources if p.is_file()}
summary['source_sha256']=hashes()
def save(): (out/'summary.json').write_text(json.dumps(summary,indent=2))
save()
counter=0
def execute(command,cwd,log,required=None):
    global counter
    counter+=1;log=Path(log)
    if str(command[0]).lower().endswith('.bat'):
        batch=Path(cwd)/f'command_{counter:02d}.cmd'
        # Vivado's Windows launcher splits unquoted plusargs on '=' even
        # though normal Windows argv parsing does not. Quote those explicitly.
        parts=[]
        for item in command:
            value=str(item).replace('\\','/')
            quoted=subprocess.list2cmdline([value])
            if '=' in value and not quoted.startswith('"'):quoted='"'+quoted+'"'
            parts.append(quoted)
        batch.write_text('@echo off\ncall '+' '.join(parts)+'\nexit /b %errorlevel%\n')
        command=['cmd.exe','/d','/c',str(batch)]
    begun=time.time()
    with log.open('w') as stream:
        completed=subprocess.run([str(x) for x in command],cwd=cwd,stdout=stream,stderr=subprocess.STDOUT)
    content=log.read_text(errors='replace')
    bad=bool(re.search(r'(?im)^\s*(?:ERROR:|Fatal:|FATAL:)',content))
    if completed.returncode or bad or (required and required not in content):
        raise RuntimeError(f'Failed command; see {log}')
    summary['evidence'].append({'log':str(log),'seconds':time.time()-begun,'required':required})
    save()
def py(script,*arguments):
    execute([sys.executable,here/script,*arguments],out,out/f'{script}_{counter}.log')
def tcl(path):return '{'+str(path).replace('\\','/').replace('}','\\}')+'}'
def clone_results(folder):
    folder.mkdir(parents=True)
    for p in fixture.iterdir():
        if p.suffix=='.npy' or p.name.startswith('temporal_'):shutil.copy2(p,folder/p.name)
def run_case(folder,snapshot,case,kind):
    results=folder/f'case{case}';clone_results(results)
    command=[vivado/'bin/xsim.bat',snapshot,'-tclbatch',out/'run_all.tcl',
             '-testplusarg',f'CASE={case}','-testplusarg',f'FIXTURE={fixture}/input.hex',
             '-testplusarg',f'OUT={results}','-testplusarg',f'TEMPORAL={fixture}/temporal_input.hex',
             '-log',results/'native.log','-wdb',results/'run.wdb']
    execute(command,folder,results/'launcher.log','PASS dsp_engine')
    if case in (7,8):
        extra=['--candidate-reference',fixture/'temporal_expectation.json'] if case==8 else []
        py('check_engine.py',results,vendor,*extra)
        if case==8:py('temporal_engine_fixture.py',results,args.temporal_model,'--check')
    summary['evidence'].append({'kind':kind,'case':case,'results':str(results)});save()
try:
    architecture=[]
    for kind,width in [('range',24),('doppler',16)]:
        ip=project/f'ip/pl_dsp/radar_fft_{kind}/radar_fft_{kind}.xci'
        raw=ip.read_text()
        values={key:re.search(r'"'+key+r'"\s*:\s*\[\s*\{\s*"value"\s*:\s*"([^"]+)"',raw).group(1) for key in ('implementation_options','C_ARCH','C_S_AXIS_CONFIG_TDATA_WIDTH')}
        if values != {'implementation_options':'radix_2_burst_io','C_ARCH':'2','C_S_AXIS_CONFIG_TDATA_WIDTH':str(width)}:raise RuntimeError(f'Approved Burst architecture mismatch: {ip}: {values}')
        architecture.append({'path':str(ip),'sha256':hashlib.sha256(ip.read_bytes()).hexdigest(),**values})
    summary['fft_architecture']=architecture;save()
    (out/'run_all.tcl').write_text('run all\nquit\n')
    py('generate_fixture.py',fixture)
    vendor.mkdir()
    package=project/'ip/pl_dsp/radar_fft_range/cmodel/xfft_v9_1_bitacc_cmodel_nt64.zip'
    with zipfile.ZipFile(package) as archive:archive.extractall(vendor)
    # Native-generated package has flat contents; reject a changed package layout.
    if not (vendor/'libIp_xfft_v9_1_bitacc_cmodel.dll').is_file():raise RuntimeError('Unsupported vendor package layout')
    py('check_engine.py',fixture,vendor,'--model-only')
    py('temporal_engine_fixture.py',fixture,args.temporal_model)
    if args.mode in ('cosim','all'):
        co=out/'cosim';co.mkdir()
        execute([vivado/'bin/xsc.bat',here/'fft_vendor_dpi.c','--gcc_compile_options',f'-I{vendor}','-o','vendor_fft'],co,co/'xsc.log','Done linking')
        rtl=sorted((project/'rtl/dsp').glob('*.sv'))
        execute([vivado/'bin/xvlog.bat','--relax','-sv',*rtl,here/'fft_vendor_axis.sv',here/'tb_dsp_engine.sv'],co,co/'compile.log')
        execute([vivado/'bin/xelab.bat','work.tb_dsp_engine','-L','xpm','--debug','off','--O3','--snapshot','dsp_cosim','--sv_root','.','--sv_lib','vendor_fft'],co,co/'elaborate.log','Built simulation snapshot')
        os.environ['RADAR_XFFT_CMODEL_DLL']=str(vendor/'libIp_xfft_v9_1_bitacc_cmodel.dll')
        for case in (1,2,3,4,5,6,7,8,9):run_case(co,'dsp_cosim',case,'OFFICIAL_CMODEL_DPI_WITH_PRODUCTION_RTL_AND_XPM')
    if args.mode in ('native','all'):
        native=out/'native';native.mkdir()
        script=out/'compile_native.tcl'
        lines=[f'create_project dsp_native {tcl(native)} -part xczu2cg-sfvc784-1-e -force',
               'set_property XPM_LIBRARIES {XPM_MEMORY XPM_CDC} [current_project]']
        for name in ('range','doppler'):lines.append(f'read_ip {tcl(project/f"ip/pl_dsp/radar_fft_{name}/radar_fft_{name}.xci")}')
        for p in sorted((project/'rtl/dsp').glob('*.sv')):lines.append(f'add_files {tcl(p)}')
        lines += [f'add_files -fileset sim_1 {tcl(here/"tb_dsp_engine.sv")}',
                  'set_property top tb_dsp_engine [get_filesets sim_1]',
                  'set_property xsim.elaborate.debug_level off [get_filesets sim_1]',
                  'set_property -name xsim.elaborate.xelab.more_options -value {--O3} -objects [get_filesets sim_1]',
                  'launch_simulation -step compile','launch_simulation -step elaborate','close_project','exit']
        script.write_text('\n'.join(lines)+'\n')
        execute([vivado/'bin/vivado.bat','-mode','batch','-source',script,'-log',native/'vivado.log','-journal',native/'vivado.jou'],out,native/'launcher.log','Built simulation snapshot')
        sim=native/'dsp_native.sim/sim_1/behav/xsim'
        for case in (1,2,3,4,5,6,9,7,8):run_case(sim,'tb_dsp_engine_behav',case,'OFFICIAL_FFT_HDL_AND_XPM_WITH_PRODUCTION_RTL')
    if hashes()!=summary['source_sha256']:raise RuntimeError('Source files changed during verification')
    summary['status']='PASS';summary['finished']=time.time();save()
except Exception as error:
    summary['status']='FAILED';summary['reason']=str(error);summary['finished']=time.time();save();raise
