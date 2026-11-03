"""T6 bounded negative tests. Isolated tempfile fixtures; never opens Vivado."""
import copy
import json
from pathlib import Path
import tempfile
import unittest
import sys
import zipfile
import xml.etree.ElementTree as ET
sys.path.insert(0, str(Path(__file__).resolve().parent))
import build_identity as ident
import release_gate as release
import hardware_export as hardware

def dump(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value), encoding='utf-8')

def smartconnect_fixture(root, native):
    paths = []
    for instance, bd in ident.SMARTCONNECT_RESETS:
        base = root/'Radar_XCZU2CG_M2.gen/sources_1/bd/radar_system/ip'/instance/'bd_0'
        xci = base/'ip/ip_1'/(bd+'_psr_aclk_0.xci')
        vhdl = base/'ip/ip_1/synth'/(bd+'_psr_aclk_0.vhd')
        parent = base/'synth'/(bd+'.v')
        constant = base/'ip/ip_0/synth'/(bd+'_one_0.v')
        dump(xci, {'ip_inst': {'parameters': {section: {name: [{'value': '0'}]
             for name in ('C_EXT_RESET_HIGH', 'C_AUX_RESET_HIGH')}
             for section in ('component_parameters', 'model_parameters')}}})
        for p, text in ((vhdl, "inst : reset_core GENERIC MAP (C_EXT_RESET_HIGH => '0', C_AUX_RESET_HIGH => '0');"),
                        (parent, f'{bd}_psr_aclk_0 psr_aclk (.ext_reset_in(one_dout), .aux_reset_in(aresetn));\n{bd}_one_0 one (.dout(one_dout));'),
                        (constant, "xlconstant #(.CONST_VAL(1'H1)) inst (.dout(dout));")):
            p.parent.mkdir(parents=True, exist_ok=True); p.write_text(text)
            native['compile_order']['synthesis_sources'].append(str(p))
        for p in (xci, vhdl, parent, constant):
            native['files'].append({'path': str(p), 'properties': {'USED_IN_SYNTHESIS': '1', 'USED_IN_IMPLEMENTATION': '1'}})
            if 'implementation_inputs' in native['compile_order']:
                native['compile_order']['implementation_inputs'].append(str(p))
        paths.append((xci, vhdl, parent, constant))
    return paths

def shared_reset_fixture():
    base = 'dut/inst/'
    specs = [('common_sys','u_common_sys_release','clk_pl_0',2),
             ('common_rx','u_rx_control_release','RADAR_RX75',11),
             ('command_sys','u_command_sys_release','clk_pl_0',1),
             ('command_rx','u_command_release','RADAR_RX75',2),
             ('delay_sys','u_phy/u_rx_epoch_sys_release','clk_pl_0',2),
             ('delay_rx','u_phy/u_rx_release','RADAR_RX75',4),
             ('cal_sys','u_phy/u_cal_epoch_sys_release','clk_pl_0',2),
             ('cal_ref','u_phy/u_cal_epoch_ref_release','clk_out1_radar_system_clk_wiz_phy_ref_0',3)]
    groups=[]
    for name,owner,clock,count in specs:
        pins=[base+name+'/consumer_'+str(i) for i in range(count)]
        groups.append({'name':name,'owner':base+owner,'clock':clock,'raw_chain_count':1,
                       'raw_source_cells':[base+'epoch_reg'],'expected_consumers':pins,
                       'consumers':[{'pin':p,'source_cells':[base+owner+'/released_reg']} for p in pins]})
    extra=['u_phy/u_ref_reset_sys_release','u_phy/u_phy_sys_release','u_phy/u_ref_release',
           'u_phy/u_activity_release','u_phy/u_bitclk_release']
    parents=sorted([base+owner+'/g_async.u_reset_sync' for _,owner,_,_ in specs]+[base+x+'/g_async.u_reset_sync' for x in extra])
    return {'shared_reset_ownership':{'status':'PASS','xpm_parents':parents,'expected_xpm_parents':parents.copy(),
            'groups':groups,'prequalified':[{'owner':base+name,'clock':'RADAR_RX75','raw_xpm_count':0,'source_cells':[base+'owner/released_reg']} for name in ('u_fifo_release','u_phy/u_deser_release')]}}

class IdentityTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='m2_t6_')
        self.root = Path(self.temp.name).resolve()
        # Cleanup is restricted to the exact new OS-temp fixture, never design.
        self.assertEqual(self.root.parent, Path(tempfile.gettempdir()).resolve())
        self.assertTrue(self.root.name.startswith('m2_t6_'))
        self.addCleanup(self.temp.cleanup)
        for name, content in [('rtl/top.v','module top; endmodule'),('sim/tb_one.sv','module tb_one; endmodule'),('constraints/top.xdc','create_clock -period 10 [get_ports clk]'),('scripts/m2_closure/recipe.tcl','# recipe'),('proj.runs/synth_1/radar_m2_top.dcp','native actual DCP')]:
            p=self.root/name; p.parent.mkdir(parents=True,exist_ok=True); p.write_text(content)
        for name in ident.REUSED_HELPERS:
            p=self.root/name; p.parent.mkdir(parents=True,exist_ok=True); p.write_text('# reused helper')
        (self.root/'proj.runs/synth_1/runme.log').write_text('INFO: synth completed\n')
        self.definition=self.root/'scripts/m2_closure/required_tests.json'
        dump(self.definition, {'tests':['tb_one'],'phase_tests':{'tb_one':['PHASE000']}})
        self.suite=self.root/'outputs/test/sim';self.suite.mkdir(parents=True)
        for name in ['tb_one','tb_one_PHASE000']:(self.suite/f'run_{name}.log').write_text('PASS tb_one\n')
        inputs={name:ident.sha(self.root/name) for name in ['rtl/top.v','sim/tb_one.sv','scripts/m2_closure/required_tests.json']}
        dump(self.suite/'input_hashes.json',inputs)
        dump(self.suite/'STATUS.json',{'state':'PASS','required_suite':True,'tests':{'tb_one':'PASS','tb_one_PHASE000':'PASS'}})
        self.sim=self.suite/'SIM_PASS.json'
        dump(self.sim,{'status':'PASS','suite':'outputs/test/sim','tests':['tb_one','tb_one_PHASE000'],'input_hashes':inputs,'log_hashes':{p.relative_to(self.root).as_posix():ident.sha(p) for p in self.suite.glob('*.log')}})
        self.native=self.root/'native.json'
        self.inventory={'schema':1,'project_root':str(self.root),'tool_version':'2025.1','project':{'PART':'xczu2cg-sfvc784-1-e'},'filesets':{'sources_1':{'TOP':'radar_m2_top'}},'runs':{'synth_1':{'DIRECTORY':str(self.root/'proj.runs/synth_1')},'impl_1':{'DIRECTORY':str(self.root/'proj.runs/impl_1')}},'files':[{'path':str(self.root/name),'properties':{'USED_IN_SYNTHESIS':'1'}} for name in ['rtl/top.v','constraints/top.xdc']]}
        for row in self.inventory['files']:row['properties']['USED_IN_IMPLEMENTATION']='1'
        self.inventory['compile_order']={'synthesis_sources':[str(self.root/'rtl/top.v')], 'synthesis_constraints':[str(self.root/'constraints/top.xdc')], 'implementation_constraints':[str(self.root/'constraints/top.xdc')], 'implementation_inputs':[row['path'] for row in self.inventory['files']]}
        smartconnect_fixture(self.root, self.inventory)
        dump(self.native,self.inventory)
        self.manifest=self.root/'build_manifest.json'
        ident.record(self.root,self.native,self.sim,self.manifest,'T6')
        self.dcp=self.root/'proj.runs/synth_1/radar_m2_top.dcp'
        self.receipt=self.root/'synth_identity.json'
        ident.seal_synth(self.root,self.native,self.sim,self.manifest,self.dcp,self.receipt)
    def check(self):return ident.require_synth(self.root,self.native,self.sim,self.manifest,self.dcp,self.receipt)
    def test_unchanged_pass(self):self.assertEqual(self.check()['status'],'PASS')
    def test_source_change(self):
        (self.root/'rtl/top.v').write_text('changed');self.assertRaises(ident.GateError,self.check)
    def test_script_change(self):
        (self.root/'scripts/m2_closure/recipe.tcl').write_text('changed');self.assertRaises(ident.GateError,self.check)
    def test_reused_package_helper_change(self):
        (self.root/ident.REUSED_HELPERS[0]).write_text('changed');self.assertRaises(ident.GateError,self.check)
    def test_reused_reopen_helper_change(self):
        (self.root/ident.REUSED_HELPERS[1]).write_text('changed');self.assertRaises(ident.GateError,self.check)
    def test_reused_common_helper_change(self):
        (self.root/ident.REUSED_HELPERS[2]).write_text('changed');self.assertRaises(ident.GateError,self.check)
    def test_native_synth_log_changed(self):
        (self.root/'proj.runs/synth_1/runme.log').write_text('changed');self.assertRaises(ident.GateError,self.check)
    def test_settings_change(self):
        self.inventory['filesets']['sources_1']['VERILOG_DEFINE']='NEW';dump(self.native,self.inventory);self.assertRaises(ident.GateError,self.check)
    def test_active_xdc_added(self):
        p=self.root/'constraints/final.xdc';p.write_text('set_input_delay 1 [get_ports data]')
        self.inventory['files'].append({'path':str(p),'properties':{'USED_IN_IMPLEMENTATION':'1'}});dump(self.native,self.inventory)
        self.assertRaises(ident.GateError,self.check)
    def test_active_xdc_removed(self):
        self.inventory['files']=[row for row in self.inventory['files'] if not row['path'].endswith('top.xdc')];dump(self.native,self.inventory);self.assertRaises(ident.GateError,self.check)
    def test_missing_implementation_inventory(self):
        self.inventory['compile_order'].pop('implementation_inputs');dump(self.native,self.inventory);self.assertRaises(ident.GateError,self.check)
    def test_implementation_xdc_omitted(self):
        self.inventory['compile_order']['implementation_constraints']=[];dump(self.native,self.inventory);self.assertRaises(ident.GateError,self.check)
    def test_implementation_input_omitted(self):
        self.inventory['compile_order']['implementation_inputs'].pop();dump(self.native,self.inventory);self.assertRaises(ident.GateError,self.check)
    def test_unregistered_rtl(self):
        (self.root/'rtl/new.v').write_text('module new;endmodule');self.assertRaises(ident.GateError,self.check)
    def test_stale_sim_gate(self):
        gate=ident.read_json(self.sim);gate['input_hashes']['rtl/top.v']='0'*64;dump(self.sim,gate);self.assertRaises(ident.GateError,self.check)
    def test_tampered_log(self):
        (self.suite/'run_tb_one.log').write_text('PASS tb_one\nchanged');self.assertRaises(ident.GateError,self.check)
    def test_omitted_required_phase(self):
        gate=ident.read_json(self.sim);gate['tests'].remove('tb_one_PHASE000');dump(self.sim,gate);self.assertRaises(ident.GateError,self.check)
    def test_failed_suite_with_old_gate(self):
        state=ident.read_json(self.suite/'STATUS.json');state['state']='FAIL';dump(self.suite/'STATUS.json',state);self.assertRaises(ident.GateError,self.check)
    def test_changed_actual_dcp(self):
        self.dcp.write_text('old or replaced DCP');self.assertRaises(ident.GateError,self.check)
    def test_published_copy_not_actual_dcp(self):
        p=self.root/'published.dcp';p.write_bytes(self.dcp.read_bytes())
        self.assertRaises(ident.GateError,ident.dcp_info,self.root,self.native,p)
    def test_reuse_manifest_output_refused(self):
        self.assertRaises(FileExistsError,ident.record,self.root,self.native,self.sim,self.manifest,'T6')

class SmartConnectResetTests(unittest.TestCase):
    """Synthetic regression fixtures for the observed b03 propagation failure."""
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='m2_t6_'); self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.native = {'files': [], 'compile_order': {'synthesis_sources': []}}
        self.paths = smartconnect_fixture(self.root, self.native)
    def check(self): return ident.smartconnect_resets(self.root, self.native)
    def change(self, path, old, new):
        text=path.read_text(); self.assertIn(old,text); path.write_text(text.replace(old,new))
    def test_good_two_instances(self): self.assertEqual(len(self.check()), 2)
    def polarity_rejected(self, name):
        for paths in self.paths:
            for section in ('component_parameters', 'model_parameters'):
                with self.subTest(instance=paths[0].name, section=section, name=name):
                    data=ident.read_json(paths[0]); original=copy.deepcopy(data)
                    data['ip_inst']['parameters'][section][name][0]['value']='1'; dump(paths[0],data)
                    self.assertRaisesRegex(ident.GateError, 'reset polarity', self.check)
                    dump(paths[0],original)
    def test_xci_external_high_rejected(self): self.polarity_rejected('C_EXT_RESET_HIGH')
    def test_xci_auxiliary_high_rejected(self): self.polarity_rejected('C_AUX_RESET_HIGH')
    def test_missing_xci_polarity(self):
        data=ident.read_json(self.paths[0][0]); del data['ip_inst']['parameters']['model_parameters']['C_EXT_RESET_HIGH']; dump(self.paths[0][0],data)
        self.assertRaisesRegex(ident.GateError, 'reset polarity', self.check)
    def test_vhdl_external_high_rejected(self):
        self.change(self.paths[0][1], "C_EXT_RESET_HIGH => '0'", "C_EXT_RESET_HIGH => '1'")
        self.assertRaisesRegex(ident.GateError, 'synthesized reset polarity', self.check)
    def test_vhdl_auxiliary_high_rejected(self):
        self.change(self.paths[1][1], "C_AUX_RESET_HIGH => '0'", "C_AUX_RESET_HIGH => '1'")
        self.assertRaisesRegex(ident.GateError, 'synthesized reset polarity', self.check)
    def test_missing_instance(self):
        self.native['files']=[row for row in self.native['files'] if 'smartconnect_0_3' not in row['path']]
        self.assertRaisesRegex(ident.GateError, 'active input missing', self.check)
    def test_duplicate_instance(self):
        self.native['files'].append(copy.deepcopy(self.native['files'][0]))
        self.assertRaisesRegex(ident.GateError, 'active input missing/duplicated', self.check)
    def test_extra_instance(self):
        self.native['files'].append({'path': str(self.root/'unexpected_psr_aclk_0.xci')})
        self.assertRaisesRegex(ident.GateError, 'instance set differs', self.check)
    def uncompiled(self, index):
        self.native['compile_order']['synthesis_sources'].remove(str(self.paths[0][index]))
        self.assertRaisesRegex(ident.GateError, 'synthesis input missing', self.check)
    def test_uncompiled_vhdl(self): self.uncompiled(1)
    def test_uncompiled_parent(self): self.uncompiled(2)
    def test_uncompiled_constant(self): self.uncompiled(3)
    def test_wrong_external_tie(self):
        self.change(self.paths[0][2], '.ext_reset_in(one_dout)', '.ext_reset_in(other_net)')
        self.assertRaisesRegex(ident.GateError, 'external reset is not', self.check)
    def test_wrong_auxiliary_reset(self):
        self.change(self.paths[0][2], '.aux_reset_in(aresetn)', '.aux_reset_in(one_dout)')
        self.assertRaisesRegex(ident.GateError, 'auxiliary reset is not', self.check)
    def test_wrong_constant_driver(self):
        self.change(self.paths[0][2], '.dout(one_dout)', '.dout(other_net)')
        self.assertRaisesRegex(ident.GateError, 'one_dout driver differs', self.check)
    def test_constant_not_one(self):
        self.change(self.paths[0][3], "1'H1", "1'H0")
        self.assertRaisesRegex(ident.GateError, 'constant is not', self.check)
    def test_comments_do_not_prove_polarity(self):
        self.change(self.paths[0][1], "C_EXT_RESET_HIGH => '0'", "C_EXT_RESET_HIGH => '1'")
        with self.paths[0][1].open('a') as f: f.write("\n-- C_EXT_RESET_HIGH => '0'\n")
        self.assertRaisesRegex(ident.GateError, 'synthesized reset polarity', self.check)

class QualificationTests(unittest.TestCase):
    def row(self):return {'instance':'dut/mb','source_stem':'dut/mb/src_hold_reg','destination_stem':'dut/mb/dst_data_reg','source_bits':[0,1],'expected_source_bits':[0,1],'destination_bits':[0,1],'expected_destination_bits':[0,1],'destination_count':2,'timed_endpoints':2,'requirement_ns':8,'worst_slack_ns':1}
    def metrics(self):return {'object_checks':'PASS','phy_same_net_pairs':3,'registered_status_checks':5,'reset_structure_checks':13,'divider_clr_check':'PASS','payloads':[self.row()],**shared_reset_fixture()}
    def test_payload_good(self):release.payload_metrics(self.metrics())
    def test_negative_max_delay(self):
        m=self.metrics();m['payloads'][0]['worst_slack_ns']=-.001;self.assertRaises(ident.GateError,release.payload_metrics,m)
    def test_nan_slack(self):
        m=self.metrics();m['payloads'][0]['worst_slack_ns']='NaN';self.assertRaises(ident.GateError,release.payload_metrics,m)
    def test_unparseable_slack(self):
        m=self.metrics();m['payloads'][0]['worst_slack_ns']='';self.assertRaises(ident.GateError,release.payload_metrics,m)
    def test_empty_payload(self):
        m=self.metrics();m['payloads']=[];self.assertRaises(ident.GateError,release.payload_metrics,m)
    def test_missing_destination_bit(self):
        m=self.metrics();m['payloads'][0]['destination_bits']=[0];self.assertRaises(ident.GateError,release.payload_metrics,m)
    def test_missing_timed_endpoint(self):
        m=self.metrics();m['payloads'][0]['timed_endpoints']=1;self.assertRaises(ident.GateError,release.payload_metrics,m)
    def test_multibit_skew_omission(self):
        m=self.metrics();m['payloads'][0]['bus_skew_required']=0;self.assertRaises(ident.GateError,release.payload_metrics,m)
    def test_scalar_skew_not_applicable(self):
        m=self.metrics();r=m['payloads'][0]
        for key in ('source_bits','expected_source_bits','destination_bits','expected_destination_bits'):r[key]=[0]
        r.update(destination_count=1,timed_endpoints=1,bus_skew_required=0)
        release.payload_metrics(m)
        self.assertEqual(release.bus_skew(self.skew(),[r])[0]['status'],'NOT_APPLICABLE_SINGLE_BIT')
    def test_event_payload_consumer(self):
        m=self.metrics();m['event_only_checks']=[{'payload_destination_registers':0,'functional_payload_consumers':1}]
        self.assertRaises(ident.GateError,release.payload_metrics,m)
    def skew(self,actual='1.000',slack='7.000'):
        return f'1. Bus Skew Report Summary\n--\n1 7 [get_cells dut/mb/src_hold_reg*]\n [get_cells dut/mb/dst_data_reg*]\n Slow 8.000 {actual} {slack}\n2. Bus Skew Report Per Constraint\n'
    def test_skew_good(self):release.bus_skew(self.skew(),[self.row()])
    def test_negative_skew(self):self.assertRaises(ident.GateError,release.bus_skew,self.skew('8.001','-0.001'),[self.row()])
    def test_nan_skew(self):self.assertRaises(ident.GateError,release.bus_skew,self.skew('NaN','NaN'),[self.row()])
    def test_missing_skew(self):self.assertRaises(ident.GateError,release.bus_skew,'1. Bus Skew Report Summary\n',[self.row()])
    def test_wrong_skew_channel(self):self.assertRaises(ident.GateError,release.bus_skew,self.skew().replace('dut/mb','wrong/mb'),[self.row()])
    def test_failed_route_old_pass(self):
        self.assertRaises(ident.GateError,release.verify_stage,{'stage':'route','state':'FAIL','build_id':'new'},{'status':'NATIVE_COMPLETE','build_id':'old'})
    def test_running_route_old_pass(self):
        self.assertRaises(ident.GateError,release.verify_stage,{'stage':'route','state':'RUNNING','build_id':'same'},{'status':'NATIVE_COMPLETE','build_id':'same'})
    def test_incomplete_routing(self):
        text='# of routable nets.... : 2\n# of fully routed nets.... : 1\n# of nets with routing errors.... : 0\n'
        self.assertRaises(ident.GateError,release.route_summary,text)
    def test_unexplained_cdc_remains(self):
        record={'rule':'CDC-10','source':'a/Q','destination':'b/D'}
        self.assertEqual(release.accepted_records([record],[],Path('.'),'CDC'),[record])
    def test_functional_cdc10_never_waived(self):
        record={'rule':'CDC-10','source':'u_system/radar_system_i/radar_capture_bd_0/inst/comb/O','destination':'dst/D'}
        self.assertRaises(ident.GateError,release.require_functional_cdc_clean,[record])
    def test_fewer_phy_pairs(self):
        m=self.metrics();m['phy_same_net_pairs']=2;self.assertRaises(ident.GateError,release.payload_metrics,m)
    def timing(self,wns='1',whs='0.1',wpws='0'):
        return f'WNS(ns) TNS(ns) Total\n----- -----\n{wns} 0 0 12 {whs} 0 0 12 {wpws} 0 0 8\n'
    def test_timing_good(self):release.timing_summary(self.timing())
    def test_negative_wns(self):self.assertRaises(ident.GateError,release.timing_summary,self.timing(wns='-0.001'))
    def test_negative_whs(self):self.assertRaises(ident.GateError,release.timing_summary,self.timing(whs='-0.001'))
    def test_negative_wpws(self):self.assertRaises(ident.GateError,release.timing_summary,self.timing(wpws='-0.001'))
    def test_nan_timing(self):self.assertRaises(ident.GateError,release.timing_summary,self.timing(wns='NaN'))

class NativeLogTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix='m2_t6_');self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name).resolve();self.reports=self.root/'reports';self.reports.mkdir()
        self.native=self.root/'native.json'
        runs={}
        for run in ('synth_1','impl_1'):
            p=self.root/'proj.runs'/run;p.mkdir(parents=True)
            (p/'runme.log').write_text('INFO: actual completed run\n')
            runs[run]={'DIRECTORY':str(p)}
        dump(self.native,{'runs':runs})
    def capture(self):return ident.capture_run_logs(self.root,self.native,self.reports,'route')
    def check(self):return release.native_log_records(self.root,self.reports)
    def test_actual_logs_good(self):
        self.capture();self.assertEqual(self.check(),[])
    def test_missing_impl_log(self):
        (self.root/'proj.runs/impl_1/runme.log').unlink();self.assertRaises(ident.GateError,self.capture)
    def test_actual_log_tamper(self):
        self.capture();(self.root/'proj.runs/impl_1/runme.log').write_text('changed');self.assertRaises(ident.GateError,self.check)
    def test_copied_log_tamper(self):
        self.capture();(self.reports/'impl_1_runme.log').write_text('changed');self.assertRaises(ident.GateError,self.check)
    def test_omitted_exact_warning(self):
        (self.root/'proj.runs/impl_1/runme.log').write_text('CRITICAL WARNING: [Route 1-2] cell exact/path\n')
        audit=self.capture();audit['logs'][1]['records']=[];dump(self.reports/'native_run_logs.json',audit)
        self.assertRaises(ident.GateError,self.check)
    def test_error_never_accepted(self):
        (self.root/'proj.runs/impl_1/runme.log').write_text('ERROR: [Route 1-2] cell exact/path\n')
        self.capture();self.assertRaises(ident.GateError,self.check)
    def test_reqp_never_accepted(self):
        (self.root/'proj.runs/impl_1/runme.log').write_text('CRITICAL WARNING: [DRC REQP-1742] exact/path\n')
        self.capture();self.assertRaises(ident.GateError,self.check)
    def test_duplicate_critical_not_count_waiver(self):
        (self.root/'proj.runs/impl_1/runme.log').write_text('CRITICAL WARNING: [Route 1-2] exact/path\n'*2)
        self.capture();rows=self.check();self.assertEqual([r['occurrence'] for r in rows],[1,2])
        self.assertEqual(release.accepted_records(rows,[],self.root,'native log'),rows)

class SharedResetTests(unittest.TestCase):
    def check(self, value):return release.shared_reset_metrics(value)
    def test_shared_reset_good(self):self.check(shared_reset_fixture())
    def test_missing_shared_reset_proof(self):self.assertRaises(ident.GateError,self.check,{})
    def test_missing_xpm_parent(self):
        m=shared_reset_fixture();m['shared_reset_ownership']['xpm_parents'].pop();self.assertRaises(ident.GateError,self.check,m)
    def test_old_endpoint_local_xpm_reappears(self):
        m=shared_reset_fixture();p=m['shared_reset_ownership'];p['xpm_parents'].append('dut/inst/u_recal_mb/g_dst_reset/u_dst_release/g_async.u_reset_sync')
        p['expected_xpm_parents']=p['xpm_parents'].copy();self.assertRaises(ident.GateError,self.check,m)
    def test_missing_epoch_group(self):
        m=shared_reset_fixture();m['shared_reset_ownership']['groups'].pop();self.assertRaises(ident.GateError,self.check,m)
    def test_duplicate_epoch_group(self):
        m=shared_reset_fixture();g=m['shared_reset_ownership']['groups'];g[-1]=copy.deepcopy(g[0]);self.assertRaises(ident.GateError,self.check,m)
    def test_two_raw_chains_same_domain(self):
        m=shared_reset_fixture();m['shared_reset_ownership']['groups'][0]['raw_chain_count']=2;self.assertRaises(ident.GateError,self.check,m)
    def test_empty_raw_chain_source(self):
        m=shared_reset_fixture();m['shared_reset_ownership']['groups'][0]['raw_source_cells']=[];self.assertRaises(ident.GateError,self.check,m)
    def test_missing_actual_reset_consumer(self):
        m=shared_reset_fixture();m['shared_reset_ownership']['groups'][0]['consumers'].pop();self.assertRaises(ident.GateError,self.check,m)
    def test_lowered_expected_consumer_population(self):
        m=shared_reset_fixture();g=m['shared_reset_ownership']['groups'][0];g['consumers'].pop();g['expected_consumers'].pop();self.assertRaises(ident.GateError,self.check,m)
    def test_independent_consumer_release(self):
        m=shared_reset_fixture();m['shared_reset_ownership']['groups'][0]['consumers'][0]['source_cells']=['dut/inst/other/released_reg'];self.assertRaises(ident.GateError,self.check,m)
    def test_wrong_shared_owner_clock(self):
        m=shared_reset_fixture();m['shared_reset_ownership']['groups'][-1]['clock']='clk_pl_0';self.assertRaises(ident.GateError,self.check,m)
    def test_d_only_cal_enable_cannot_replace_owner(self):
        m=shared_reset_fixture();g=m['shared_reset_ownership']['groups'][-1];g['owner']='dut/inst/u_phy/u_cal_enable_ref';self.assertRaises(ident.GateError,self.check,m)
    def test_fifo_reintroduces_raw_xpm(self):
        m=shared_reset_fixture();m['shared_reset_ownership']['prequalified'][0]['raw_xpm_count']=1;self.assertRaises(ident.GateError,self.check,m)
    def test_deser_hold_wrong_domain(self):
        m=shared_reset_fixture();m['shared_reset_ownership']['prequalified'][1]['clock']='clk_pl_0';self.assertRaises(ident.GateError,self.check,m)

class ResetTimingTests(unittest.TestCase):
    def metrics(self):
        rows=[]
        kinds={'u_serdes_div':'DIVIDER_CLR','u_rx_div':'DIVIDER_CLR','delay0':'PRIMITIVE_RST','delay1':'PRIMITIVE_RST','delay2':'PRIMITIVE_RST','ser_fr':'PRIMITIVE_RST','ser_d0':'PRIMITIVE_RST','ser_d1':'PRIMITIVE_RST','idelayctrl':'CONTROLLER_RST'}
        for label,kind in kinds.items():
            endpoint=label+'/RST'
            row={'label':label,'kind':kind,'endpoint':endpoint,'source_cells':['source_reg'],'bound_ns':8,'primitive_ref':'IDELAYE3' if label.startswith('delay') else 'ISERDESE3','native_library_checks':[] if kind=='PRIMITIVE_RST' else ['recovery','removal'],'reset_propagation_arcs':1,'paths':[{'delay_type':mode,'native_check':'NATIVE','startpoint':'source_reg/C','endpoint':endpoint,'slack_ns':1,'datapath_ns':2,'requirement_ns':3,'endpoint_clock_delay_ns':1,'exception':''} for mode in ('max','min')]}
            if kind=='PRIMITIVE_RST':row['paths'][1].update(native_check='NO_LIBRARY_CHECK',slack_ns=None,requirement_ns=None,raw_slack='',raw_requirement='')
            rows.append(row)
        return {'phy_reset_timing':{'status':'PASS','shared_clock_gate':'PASS','clock_gate_clear_arc_audit':'PASS','guard_after_propagation_ns':45.328,'clock_clear_arcs':[{'buffer':n,'max_fall_ns':.213} for n in ('div1','div4')],'clock_leaf_delays':[{'pin':str(n)+'/CLK','max_ns':1,'estimated':0} for n in range(12)],'clock_tree_max_ns':1,'clock_close_guard_ns':26.664,'clock_close_margin_ns':17.451,'endpoints':rows}}
    def test_reset_good(self):release.reset_metrics(self.metrics())
    def test_reset_missing_endpoint(self):
        m=self.metrics();m['phy_reset_timing']['endpoints'].pop();self.assertRaises(ident.GateError,release.reset_metrics,m)
    def test_reset_missing_min(self):
        m=self.metrics();m['phy_reset_timing']['endpoints'][0]['paths'].pop();self.assertRaises(ident.GateError,release.reset_metrics,m)
    def test_reset_negative_removal(self):
        m=self.metrics();m['phy_reset_timing']['endpoints'][0]['paths'][1]['slack_ns']=-.001;self.assertRaises(ident.GateError,release.reset_metrics,m)
    def test_reset_nan_delay(self):
        m=self.metrics();m['phy_reset_timing']['endpoints'][0]['paths'][0]['datapath_ns']='NaN';self.assertRaises(ident.GateError,release.reset_metrics,m)
    def test_reset_unbounded_propagation(self):
        m=self.metrics();m['phy_reset_timing']['endpoints'][0]['paths'][0]['datapath_ns']=8.001;self.assertRaises(ident.GateError,release.reset_metrics,m)
    def test_reset_false_path(self):
        m=self.metrics();m['phy_reset_timing']['endpoints'][0]['paths'][1]['exception']='False Path';self.assertRaises(ident.GateError,release.reset_metrics,m)
    def test_reset_missing_library_clear_delay(self):
        m=self.metrics();m['phy_reset_timing']['clock_gate_clear_arc_audit']='PENDING_NATIVE_LIBRARY_REVIEW';self.assertRaises(ident.GateError,release.reset_metrics,m)
    def test_reset_clock_close_margin_negative(self):
        m=self.metrics();m['phy_reset_timing']['clock_close_margin_ns']=-.001;self.assertRaises(ident.GateError,release.reset_metrics,m)
    def test_reset_clock_clear_arc_missing(self):
        m=self.metrics();m['phy_reset_timing']['clock_clear_arcs'].pop();self.assertRaises(ident.GateError,release.reset_metrics,m)
    def test_pre_route_negative_native_slack_is_observation(self):
        m=self.metrics();r=m['phy_reset_timing'];r['status']='PRE_ROUTE';r['endpoints'][0]['paths'][0]['slack_ns']=-.131
        release.reset_metrics(m,'synth')
    def test_pre_route_cannot_release(self):
        m=self.metrics();m['phy_reset_timing']['status']='PRE_ROUTE';self.assertRaises(ident.GateError,release.reset_metrics,m)
    def test_pre_route_nan_still_fails(self):
        m=self.metrics();r=m['phy_reset_timing'];r['status']='PRE_ROUTE';r['endpoints'][0]['paths'][0]['slack_ns']='NaN'
        self.assertRaises(ident.GateError,release.reset_metrics,m,'synth')
    def test_primitive_fabricated_zero_min_fails(self):
        m=self.metrics();m['phy_reset_timing']['endpoints'][2]['paths'][1]['slack_ns']=0;self.assertRaises(ident.GateError,release.reset_metrics,m)
    def test_primitive_missing_library_proof(self):
        m=self.metrics();m['phy_reset_timing']['endpoints'][2]['reset_propagation_arcs']=0;self.assertRaises(ident.GateError,release.reset_metrics,m)
    def test_native_check_cannot_be_marked_absent(self):
        m=self.metrics();m['phy_reset_timing']['endpoints'][0]['paths'][1]['native_check']='NO_LIBRARY_CHECK';self.assertRaises(ident.GateError,release.reset_metrics,m)
    def test_missing_clock_leaf_delay(self):
        m=self.metrics();m['phy_reset_timing']['clock_leaf_delays'].pop();self.assertRaises(ident.GateError,release.reset_metrics,m)
    def test_final_clock_delay_estimate_fails(self):
        m=self.metrics();m['phy_reset_timing']['clock_leaf_delays'][0]['estimated']=1;self.assertRaises(ident.GateError,release.reset_metrics,m)

class HardwareExportTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='m2_t6_export_')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.hwh = self.root/'radar_system.hwh'
        tree = ET.Element('SYSTEM'); modules = ET.SubElement(tree, 'MODULES')
        dma = ET.SubElement(modules, 'MODULE', MODTYPE='axi_dma', INSTANCE='axi_dma_capture')
        gpio = ET.SubElement(modules, 'MODULE', MODTYPE='axi_gpio', INSTANCE='axi_gpio_ctrl')
        ps = ET.SubElement(modules, 'MODULE', MODTYPE='zynq_ultra_ps_e', INSTANCE='zynq_ultra_ps_e_0')
        for module, values in ((dma, {'C_INCLUDE_SG':1,'C_INCLUDE_MM2S':0,'C_INCLUDE_S2MM':1,'C_S_AXIS_S2MM_TDATA_WIDTH':64,'C_M_AXI_S2MM_DATA_WIDTH':64}),
                               (gpio, {'C_IS_DUAL':1,'C_GPIO_WIDTH':32,'C_GPIO2_WIDTH':32})):
            params = ET.SubElement(module, 'PARAMETERS')
            for key,value in values.items(): ET.SubElement(params,'PARAMETER',NAME=key,VALUE=str(value))
        for interface in ('M_AXI_SG','M_AXI_S2MM'):
            ET.SubElement(dma,'MEMRANGE',MASTERBUSINTERFACE=interface,SLAVEBUSINTERFACE='S_AXI_HP0_FPD',BASEVALUE='0x00000000',HIGHVALUE='0x7FFFFFFF')
        for instance,interface,base,high in (('axi_dma_capture','S_AXI_LITE','0xA0000000','0xA000FFFF'),('axi_gpio_ctrl','S_AXI','0xA0010000','0xA001FFFF')):
            ET.SubElement(ps,'MEMRANGE',INSTANCE=instance,SLAVEBUSINTERFACE=interface,BASEVALUE=base,HIGHVALUE=high)
        self.data = ET.tostring(tree)
        self.hwh.write_bytes(self.data)
        self.bit = self.root/'radar_system_m2.bit'; self.bit.write_bytes(b'current bitstream fixture')
    def gate(self):
        return {'internal_status':'PASS','SIMULATION':'PASS','NATIVE_ROUTE_EXECUTION':'PASS','BITSTREAM_ELIGIBLE':'YES',
                'SOURCE_TIMING':'VERIFIED_WITH_SCOPE','BOARD_ELECTRICAL':'CONFIRMED_FOR_THIS_BOARD','BOARD_TEST':'NOT_RUN'}
    def make_xsa(self, mode='dev-hw', extra=None, data=None):
        p = self.root/('radar_system_m2.xsa' if mode == 'candidate' else 'radar_system_m2_dev_hw.xsa')
        with zipfile.ZipFile(p,'w') as z:
            z.writestr('radar_system.hwh',self.data if data is None else data)
            if mode == 'candidate':z.writestr('radar_m2_top.bit',self.bit.read_bytes())
            for name,value in (extra or {}).items():z.writestr(name,value)
        return p
    def test_candidate_does_not_require_prior_board_test(self):hardware.eligible(self.gate(),'candidate')
    def test_pending_development_allowed(self):
        g=self.gate();g.update(BITSTREAM_ELIGIBLE='NO',SOURCE_TIMING='PENDING',BOARD_ELECTRICAL='PENDING')
        hardware.eligible(g,'dev-hw')
    def test_pending_candidate_prohibited(self):
        g=self.gate();g['SOURCE_TIMING']='PENDING';self.assertRaises(ident.GateError,hardware.eligible,g,'candidate')
    def test_false_bitstream_eligibility(self):
        g=self.gate();g['BITSTREAM_ELIGIBLE']='NO';self.assertRaises(ident.GateError,hardware.eligible,g,'candidate')
    def test_internal_failure_blocks_development(self):
        g=self.gate();g['internal_status']='FAIL';self.assertRaises(ident.GateError,hardware.eligible,g,'dev-hw')
    def test_missing_simulation_blocks_development(self):
        g=self.gate();g['SIMULATION']='NOT_RUN';self.assertRaises(ident.GateError,hardware.eligible,g,'dev-hw')
    def test_development_xsa_is_bit_free(self):
        r=hardware.inspect_xsa(self.make_xsa(),'dev-hw',self.hwh);self.assertIsNone(r['embedded_bit'])
    def test_development_with_bit_rejected(self):
        self.assertRaises(ident.GateError,hardware.inspect_xsa,self.make_xsa(extra={'old.bit':b'old'}),'dev-hw',self.hwh)
    def test_development_with_bin_rejected(self):
        self.assertRaises(ident.GateError,hardware.inspect_xsa,self.make_xsa(extra={'image.bin':b'old'}),'dev-hw',self.hwh)
    def test_development_with_mcs_rejected(self):
        self.assertRaises(ident.GateError,hardware.inspect_xsa,self.make_xsa(extra={'image.mcs':b'old'}),'dev-hw',self.hwh)
    def test_xsa_unsafe_member_rejected(self):
        self.assertRaises(ident.GateError,hardware.inspect_xsa,self.make_xsa(extra={'../escaped':b'bad'}),'dev-hw',self.hwh)
    def test_candidate_same_embedded_bit(self):
        r=hardware.inspect_xsa(self.make_xsa('candidate'),'candidate',self.hwh,self.bit);self.assertEqual(r['embedded_bit'],'radar_m2_top.bit')
    def test_candidate_changed_standalone_bit_rejected(self):
        p=self.make_xsa('candidate');self.bit.write_bytes(b'different');self.assertRaises(ident.GateError,hardware.inspect_xsa,p,'candidate',self.hwh,self.bit)
    def test_candidate_two_bits_rejected(self):
        p=self.make_xsa('candidate',extra={'extra.bit':b'extra'});self.assertRaises(ident.GateError,hardware.inspect_xsa,p,'candidate',self.hwh,self.bit)
    def test_changed_native_hwh_rejected(self):
        p=self.make_xsa();self.hwh.write_bytes(self.data+b'\n');self.assertRaises(ident.GateError,hardware.inspect_xsa,p,'dev-hw',self.hwh)
    def test_simple_dma_export_rejected(self):
        data=self.data.replace(b'NAME="C_INCLUDE_SG" VALUE="1"',b'NAME="C_INCLUDE_SG" VALUE="0"')
        self.assertRaises(ident.GateError,hardware.hwh_contract,data)
    def test_wrong_gpio_address_rejected(self):
        self.assertRaises(ident.GateError,hardware.hwh_contract,self.data.replace(b'0xA0010000',b'0xA0020000'))
    def test_missing_sg_ddr_map_rejected(self):
        self.assertRaises(ident.GateError,hardware.hwh_contract,self.data.replace(b'M_AXI_SG',b'M_AXI_WRONG'))
    def test_wrong_s2mm_width_rejected(self):
        data=self.data.replace(b'NAME="C_S_AXIS_S2MM_TDATA_WIDTH" VALUE="64"',b'NAME="C_S_AXIS_S2MM_TDATA_WIDTH" VALUE="32"')
        self.assertRaises(ident.GateError,hardware.hwh_contract,data)
    def test_ambiguous_xsa_name_rejected(self):
        p=self.make_xsa();q=p.with_name('anything.xsa');p.rename(q)
        self.assertRaises(ident.GateError,hardware.inspect_xsa,q,'dev-hw',self.hwh)

class NativeExportLogTests(unittest.TestCase):
    def log(self):
        return ('# Vivado v2025.1 (64-bit)\n'
                '# Command line : vivado.exe -mode batch -source build_export.tcl -tclargs dev-hw BUILD_02 SIM_PASS.json\n'
                'INFO: Current native export completed\n'
                'M2_EXPORT_NATIVE_COMPLETE dev-hw BUILD_02\n'
                'INFO: [Common 17-206] Exiting Vivado at Wed Oct 14 12:00:00 2026...\n')
    def check(self, content=None, mode='dev-hw', build='BUILD_02', code=0):
        return hardware.native_session_log(self.log() if content is None else content,mode,build,code)
    def test_complete_native_session_passes(self):self.check()
    def test_native_session_nonzero_exit(self):self.assertRaises(ident.GateError,self.check,code=1)
    def test_native_session_exit_code_missing(self):self.assertRaises(ident.GateError,self.check,code=None)
    def test_native_session_missing_exit(self):
        self.assertRaises(ident.GateError,self.check,self.log().split('INFO: [Common 17-206]')[0])
    def test_native_session_truncated_exit(self):self.assertRaises(ident.GateError,self.check,self.log()[:-7])
    def test_native_session_wrong_mode(self):self.assertRaises(ident.GateError,self.check,mode='candidate')
    def test_native_session_wrong_build(self):self.assertRaises(ident.GateError,self.check,build='OLD')
    def test_native_session_wrong_version(self):self.assertRaises(ident.GateError,self.check,self.log().replace('v2025.1','v2024.2'))
    def test_native_session_without_marker(self):
        self.assertRaises(ident.GateError,self.check,self.log().replace('M2_EXPORT_NATIVE_COMPLETE dev-hw BUILD_02\n',''))
    def test_native_session_duplicate_marker(self):
        self.assertRaises(ident.GateError,self.check,self.log().replace('M2_EXPORT_NATIVE_COMPLETE dev-hw BUILD_02\n','M2_EXPORT_NATIVE_COMPLETE dev-hw BUILD_02\n'*2))
    def test_native_session_tcl_source_echo_is_not_marker(self):
        self.assertRaises(ident.GateError,self.check,self.log().replace('M2_EXPORT_NATIVE_COMPLETE dev-hw BUILD_02','# M2_EXPORT_NATIVE_COMPLETE dev-hw BUILD_02'))
    def test_native_session_error(self):
        self.assertRaises(ident.GateError,self.check,self.log().replace('INFO: Current','ERROR: [Common 17-1] failure\nINFO: Current'))
    def test_native_session_critical(self):
        self.assertRaises(ident.GateError,self.check,self.log().replace('INFO: Current','CRITICAL WARNING: [DRC 1-2] failure\nINFO: Current'))
    def test_native_session_extra_tail(self):self.assertRaises(ident.GateError,self.check,self.log()+'late error\n')
    def test_native_session_missing_header(self):self.assertRaises(ident.GateError,self.check,self.log().replace('# Vivado v2025.1 (64-bit)\n',''))

if __name__ == '__main__':
    unittest.main(verbosity=2)
