"""Host-only negative and arithmetic tests using synthetic evidence in TemporaryDirectory.
These fixtures never constitute actual board, radar firmware or STA qualification.
"""
import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parent))
import source_timing as st

class SourceTimingTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='m2_r07_test_')
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.path = self.root / st.CONTRACT
        self.path.parent.mkdir(parents=True)
        self.value = st.read(Path(__file__).with_name('R07_contract.json'))
        (self.root/'fixture.txt').write_text('SYNTHETIC HOST TEST ONLY: no physical qualification.\n', encoding='utf-8')
        self.value['evidence'] = [{'id':'fixture', 'path':'fixture.txt', 'sha256':st.sha(self.root/'fixture.txt'), 'kind':'synthetic_host_test'}]

    def save(self):
        self.path.write_text(json.dumps(self.value), encoding='utf-8')

    def source(self):
        s = self.value['source_timing']
        s['status'] = 'VERIFIED_WITH_SCOPE'
        s['review'] = {'reviewer':'test reviewer','date':'2026-10-14','scope':'test scope only','evidence_refs':['fixture']}
        for key in ('reference_plane','operating_conditions','relative_uncertainty_accounting','path_coverage'):
            s[key] = 'Synthetic host unit test explanation'
        s['no_double_counting_reviewed'] = True
        f = s['firmware']
        for key in ('version','cli_version','output_mode','lane_mapping_and_polarity','frame_clock_relation'):
            f[key] = 'test-only identity'
        f.update(bin_evidence='fixture',cfg_evidence='fixture',confirmed_running_on_this_board=True)
        s['idelay_scope']['evidence_refs'] = ['fixture']
        for port in st.PORTS:
            for edge in st.EDGES:
                s['lanes'][port][edge] = dict(source_min_ns=-.2,source_max_ns=.25,
                    data_path_min_ns=.15,data_path_max_ns=.2,clock_path_min_ns=.1,clock_path_max_ns=.18,
                    early_margin_ns=.03,late_margin_ns=.04,reference_edge_relation='synthetic clock edge relation',
                    evidence_refs={c:['fixture'] for c in ('source','path','margin')})
        return s

    def board(self):
        b = self.value['board_electrical']
        b['status'] = 'CONFIRMED_FOR_THIS_BOARD'
        b['review'] = {'reviewer':'test reviewer','date':'2026-10-14','scope':'test scope only','evidence_refs':['fixture']}
        for key in ('f20_core_revision','f20_carrier_revision','adapter_revision','radar_revision','assembly_identity','power_ground_polarity_review','power_off_compatibility_review','bank64_ac_bias_external_termination'):
            b[key] = 'Synthetic host fixture only'
        b.update(actual_board_confirmed=True,bank65_vcco_v=1.8,bank65_confirmation='JUMPER_CONFIRMED_BY_USER',bank64_vcco_v=1.2,bank44_vcco_v=3.3)
        for bank in ('bank65','bank64','bank44'):
            b[bank+'_evidence_refs']=['fixture']
        for seg in b['bank65_termination']['segments'].values():
            seg.update(external_diff_ohms=[],review='test whole-path review',evidence_refs=['fixture'])
        return b

    def assert_bad(self):
        self.save()
        with self.assertRaises((st.TimingError, ValueError)):
            st.assess_contract(self.root)

    def native(self, qualified=True):
        if qualified:
            self.source(); self.board()
        self.save(); result=st.assess_contract(self.root)
        reports=self.root/'reports'; reports.mkdir()
        xdc=st.render_xdc(result) if qualified else ''
        (reports/'r07_effective_timing.xdc').write_text(xdc,encoding='utf-8')
        if qualified:
            (self.root/'constraints').mkdir()
            (self.root/st.XDC).write_text(xdc,encoding='utf-8')
        props={}
        for name,ball in zip(('radar_bitclk_p','radar_bitclk_n','radar_frclk_p','radar_frclk_n','radar_data0_p','radar_data0_n','radar_data1_p','radar_data1_n'),('L7','L6','L3','L2','K8','K7','J6','H6')):
            props[name]={'PACKAGE_PIN':ball,'IOSTANDARD':'LVDS','DIFF_TERM_ADV':'TERM_100'}
        for name,ball in (('phy_ref_clk_100m_clk_p','AE5'),('phy_ref_clk_100m_clk_n','AF5')):
            props[name]={'PACKAGE_PIN':ball,'IOSTANDARD':'DIFF_SSTL12','DIFF_TERM_ADV':''}
        for name,ball in (('radar_uart_tx','AG10'),('radar_uart_rx','AH10'),('radar_nerr_n','AE12'),('radar_3v3_present','AF12'),('radar_sync','AE10'),('radar_nrst_n','AF10')):
            props[name]={'PACKAGE_PIN':ball,'IOSTANDARD':'LVCMOS33'}
        ibufs={}
        for leaf,stem in (('u_bclk_ibuf','radar_bitclk'),('u_fr_ibuf','radar_frclk'),('u_d0_ibuf','radar_data0'),('u_d1_ibuf','radar_data1')):
            ibufs[leaf]={'REF_NAME':'IBUFDS','IOSTANDARD':'LVDS','DIFF_TERM':'0','I_PORT':stem+'_p','IB_PORT':stem+'_n'}
        clocks={}
        # Values observed in Vivado 2025.1: generated /4 multiplies the actual
        # 3.333 ns native clock, not the unquantized ideal 3.333333 ns literal.
        for name,period,half in (('RADAR_BCLK300','3.333','1.667'),('RADAR_SER300','3.333','1.667'),('RADAR_RX75','13.332','6.666')):
            clocks[name]={'PERIOD':period,'WAVEFORM':'0.000 '+half,'IS_GENERATED':'0' if name=='RADAR_BCLK300' else '1','MASTER_CLOCK':'' if name=='RADAR_BCLK300' else 'RADAR_BCLK300'}
        rows=[]
        for port in st.PORTS:
            for edge in st.EDGES:
                for bound in ('min','max'):
                    worst={}
                    if qualified:
                        worst={'STARTPOINT_PIN':port,'ENDPOINT_PIN':st.BASE+st.SERDES[port]+'/D',
                               'STARTPOINT_CLOCK':'RADAR_BCLK300','ENDPOINT_CLOCK':'RADAR_SER300',
                               'STARTPOINT_CLOCK_EDGE':'0.000' if edge=='rise' else '1.667',
                               'INPUT_DELAY':str(result['effective_delays'][port][edge][bound]),'DELAY_TYPE':bound,'EXCEPTION':'','SLACK':'0.500'}
                    rows.append({'port':port,'edge':edge,'bound':bound,'path_count':1 if qualified else 0,'ignored_path_count':0,'worst':worst})
                    (reports/f'r07_{port}_{edge}_{bound}.rpt').write_text('Slack (MET) : 0.500000ns\n' if qualified else 'NO_CONSTRAINED_INPUT_PATH',encoding='utf-8')
        self.nvalue={'schema':1,'tool_version':'2025.1','stage':'route','active_source_xdc':qualified,'active_template_xdc':False,
                     'ports':props,'ibufs':ibufs,'clocks':clocks,'paths':rows,
                     'delay_cells':{st.BASE+f'g_lane[{lane}].u_delay':{'DELAY_FORMAT':'TIME','DELAY_TYPE':'VAR_LOAD','DELAY_VALUE':'0'} for lane in (0,1,2)}}
        self.reports=reports
        self.save_native()
        return self.nvalue

    def save_native(self):
        (self.reports/'r07_native.json').write_text(json.dumps(self.nvalue),encoding='utf-8')

    def native_bad(self):
        self.save_native()
        with self.assertRaises((st.TimingError, ValueError)):
            st.qualify_r07(self.root,self.reports)

    def test_pending_no_xdc(self):
        self.save(); result=st.assess_contract(self.root)
        self.assertFalse(result['eligible'])
        with self.assertRaises(st.TimingError): st.render_xdc(result)

    def test_source_and_board_independent(self):
        self.source(); self.save(); result=st.assess_contract(self.root)
        self.assertEqual(result['SOURCE_TIMING'],'VERIFIED_WITH_SCOPE')
        self.assertEqual(result['BOARD_ELECTRICAL'],'PENDING')
        self.assertFalse(result['eligible'])
        values=st.effective_input_delays(st.render_xdc(result))
        self.assertEqual(len(values),12)
        self.assertAlmostEqual(values[(st.PORTS[0],'rise','min')],-.26)
        self.assertAlmostEqual(values[(st.PORTS[0],'fall','max')],.39)

    def test_both_reviewed_fixture_eligible(self):
        self.source(); self.board(); self.save()
        self.assertTrue(st.assess_contract(self.root)['eligible'])

    def test_outward_ps_rounding_preserves_raw(self):
        s=self.source(); s['lanes'][st.PORTS[0]]['rise']['source_min_ns']=-.2004
        s['lanes'][st.PORTS[0]]['rise']['source_max_ns']=.2504
        self.save(); row=st.assess_contract(self.root)['effective_delays'][st.PORTS[0]]['rise']
        self.assertAlmostEqual(row['raw_min'],-.2604)
        self.assertAlmostEqual(row['raw_max'],.3904)
        self.assertEqual(row['min'],-.261)
        self.assertEqual(row['max'],.391)

    def test_nonfinite_json(self):
        s=self.source(); s['lanes'][st.PORTS[0]]['rise']['source_min_ns']=float('nan'); self.assert_bad()

    def test_infinity(self):
        s=self.source(); s['lanes'][st.PORTS[0]]['rise']['late_margin_ns']=float('inf'); self.assert_bad()

    def test_boolean_not_number(self):
        s=self.source(); s['lanes'][st.PORTS[0]]['rise']['source_min_ns']=True; self.assert_bad()

    def test_reversed_source_window(self):
        s=self.source(); s['lanes'][st.PORTS[0]]['rise']['source_min_ns']=1; self.assert_bad()

    def test_reversed_data_path(self):
        s=self.source(); s['lanes'][st.PORTS[0]]['fall']['data_path_min_ns']=1; self.assert_bad()

    def test_negative_margin(self):
        s=self.source(); s['lanes'][st.PORTS[0]]['fall']['late_margin_ns']=-.1; self.assert_bad()

    def test_missing_lane(self):
        s=self.source(); del s['lanes']['radar_frclk_p']; self.assert_bad()

    def test_negative_port_not_lane(self):
        s=self.source(); s['lanes']['radar_data0_n']=s['lanes'].pop('radar_data0_p'); self.assert_bad()

    def test_missing_clock_edge(self):
        s=self.source(); del s['lanes'][st.PORTS[0]]['fall']; self.assert_bad()

    def test_source_hash_tamper(self):
        self.source(); (self.root/'fixture.txt').write_text('changed'); self.assert_bad()

    def test_unbound_budget_evidence(self):
        s=self.source(); s['lanes'][st.PORTS[0]]['rise']['evidence_refs']['path']=[]; self.assert_bad()

    def test_evidence_escape(self):
        self.source(); self.value['evidence'][0]['path']='../outside.txt'; self.assert_bad()

    def test_firmware_not_actual(self):
        s=self.source(); s['firmware']['confirmed_running_on_this_board']=False; self.assert_bad()

    def test_source_rate_mismatch(self):
        s=self.source(); s['firmware']['rate_mbps_per_lane']=900; self.assert_bad()

    def test_double_counting_unreviewed(self):
        s=self.source(); s['no_double_counting_reviewed']=False; self.assert_bad()

    def test_arbitrary_dynamic_taps_not_qualified(self):
        s=self.source(); s['idelay_scope']['mode']='ALL_0_TO_511'; self.assert_bad()

    def test_physical_not_confirmed(self):
        self.source(); b=self.board(); b['actual_board_confirmed']=False; self.assert_bad()

    def test_vcco_wrong(self):
        self.source(); b=self.board(); b['bank65_vcco_v']=1.2; self.assert_bad()

    def test_incomplete_termination_chain(self):
        self.source(); b=self.board(); del b['bank65_termination']['segments']['radar_board']; self.assert_bad()

    def test_parallel_termination_conflict(self):
        self.source(); b=self.board(); b['bank65_termination']['segments']['radar_board']['external_diff_ohms']=[100]; self.assert_bad()

    def test_ddr_fall_requires_add_delay(self):
        self.source(); self.save(); content=st.render_xdc(st.assess_contract(self.root))
        with self.assertRaises(st.TimingError): st.effective_input_delays(content.replace(' -add_delay',''))

    def test_native_duplicate_declaration(self):
        self.source(); self.save(); content=st.render_xdc(st.assess_contract(self.root))
        with self.assertRaises(st.TimingError): st.effective_input_delays(content+'\n'+content)

    def test_native_reference_clock_mismatch(self):
        self.source(); self.save(); content=st.render_xdc(st.assess_contract(self.root))
        with self.assertRaises(st.TimingError): st.effective_input_delays(content.replace('get_clocks RADAR_BCLK300','get_clocks REF300'))

    def test_native_complete_host_fixture(self):
        self.native(); self.assertTrue(st.qualify_r07(self.root,self.reports)['eligible'])

    def test_native_pending_no_paths_is_not_pass(self):
        self.native(False); result=st.qualify_r07(self.root,self.reports)
        self.assertFalse(result['eligible']); self.assertEqual(result['native_status'],'PENDING_SOURCE_CONSTRAINTS')

    def test_native_pending_delay_under_other_filename(self):
        self.native(False)
        (self.reports/'r07_effective_timing.xdc').write_text('set_input_delay -clock [get_clocks RADAR_BCLK300] -min 0 [get_ports radar_data0_p]\n')
        self.native_bad()

    def test_native_port_termination_missing(self):
        v=self.native(); v['ports']['radar_data0_p']['DIFF_TERM_ADV']='TERM_NONE'; self.native_bad()

    def test_native_ibuf_polarity_swap(self):
        v=self.native(); v['ibufs']['u_d0_ibuf']['I_PORT']='radar_data0_n'; self.native_bad()

    def test_native_clock_wrong_master(self):
        v=self.native(); v['clocks']['RADAR_SER300']['MASTER_CLOCK']='REF300'; self.native_bad()

    def test_native_clock_wrong_rate(self):
        v=self.native(); v['clocks']['RADAR_BCLK300']['PERIOD']='10.000'; self.native_bad()

    def test_native_rx_uses_actual_master_period(self):
        v=self.native(); v['clocks']['RADAR_RX75']['PERIOD']='13.333'; self.native_bad()

    def test_native_duplicated_launch_edge_label(self):
        v=self.native(); v['paths'][2]['worst']['STARTPOINT_CLOCK_EDGE']='0.000'; self.native_bad()

    def test_native_wrong_source_clock(self):
        v=self.native(); v['paths'][0]['worst']['STARTPOINT_CLOCK']='REF300'; self.native_bad()

    def test_native_path_false_path(self):
        v=self.native(); v['paths'][0]['ignored_path_count']=1; self.native_bad()

    def test_native_empty_min_path(self):
        v=self.native(); v['paths'][0]['path_count']=0; self.native_bad()

    def test_native_wrong_input_delay(self):
        v=self.native(); v['paths'][0]['worst']['INPUT_DELAY']='0.000'; self.native_bad()

    def test_native_path_wrong_endpoint(self):
        v=self.native(); v['paths'][0]['worst']['ENDPOINT_PIN']=st.BASE+'u_fr_ser/D'; self.native_bad()

    def test_native_rounding_cannot_hide_negative_slack(self):
        self.native(); self.nvalue['paths'][0]['worst']['SLACK']='-0.000'
        (self.reports/'r07_radar_data0_p_rise_min.rpt').write_text('Slack (VIOLATED) : -0.000100ns\n')
        self.native_bad()

    def test_native_wrong_idelay_point(self):
        v=self.native(); v['delay_cells'][st.BASE+'g_lane[0].u_delay']['DELAY_VALUE']='100'; self.native_bad()

if __name__ == '__main__': unittest.main(verbosity=2)
