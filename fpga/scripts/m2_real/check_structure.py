from pathlib import Path
import json,xml.etree.ElementTree as ET,csv,re
root=Path(__file__).resolve().parents[2]
delivery=root/'delivery/20261012_171114'
baseline=delivery/'before/radar_system.bd'
if not baseline.exists():baseline=root.parent/'evidence/delivery/before/radar_system.bd'
delivery.mkdir(parents=True,exist_ok=True)
b=json.loads(baseline.read_text())['design']
a=json.loads((root/'Radar_XCZU2CG_M2.srcs/sources_1/bd/radar_system/radar_system.bd').read_text())['design']
assert set(a['components'])==set(b['components']) and len(a['components'])==12
assert [n for n,c in a['components'].items() if 'module_ref:radar_capture_bd:' in c.get('vlnv','')]==['radar_capture_bd_0']
diff=[]
for n,c in b['components'].items():
 if n=='radar_capture_bd_0':continue
 for p,v in c.get('parameters',{}).items():
  av=a['components'][n].get('parameters',{}).get(p,{}).get('value')
  if v.get('value')!=av:diff.append((n,p,v.get('value'),av))
assert not diff,diff
def endpoints(net):return set(net.get('pins',[])+net.get('ports',[])+net.get('interface_ports',[]))
def findnets(design,kind):return {n:endpoints(v) for n,v in design[kind].items()}
signals=findnets(a,'nets'); interfaces=findnets(a,'interface_nets')
for n,pts in findnets(b,'interface_nets').items():assert interfaces[n]==pts,(n,pts,interfaces[n])
wires=[
('zynq_ultra_ps_e_0/pl_clk0','radar_capture_bd_0/clk_sys',1),
('rst_sys_100m/peripheral_aresetn','radar_capture_bd_0/rstn_sys',1),
('clk_wiz_phy_ref/clk_out1','radar_capture_bd_0/clk_ref',1),
('rst_phy_ref_300m/peripheral_reset','radar_capture_bd_0/rst_ref',1),
('axi_dma_capture/s2mm_prmry_reset_out_n','radar_capture_bd_0/dma_stream_resetn',1),
('axi_gpio_ctrl/gpio_io_o','radar_capture_bd_0/ctrl_i',32),
('radar_capture_bd_0/status_o','axi_gpio_ctrl/gpio2_io_i',32),
('zynq_ultra_ps_e_0/emio_uart1_txd','radar_capture_bd_0/ps_uart_tx_i',1),
('radar_capture_bd_0/ps_uart_rx_o','zynq_ultra_ps_e_0/emio_uart1_rxd',1),
('radar_capture_bd_0/rx_clk_o','axis_fifo_capture/s_axis_aclk',1),
('radar_capture_bd_0/capture_resetn','axis_fifo_capture/s_axis_aresetn',1),
('axis_fifo_capture/axis_wr_data_count','radar_capture_bd_0/fifo_wr_count',32),
('axis_fifo_capture/prog_full','radar_capture_bd_0/fifo_prog_full',1),
('axis_fifo_capture/almost_full','radar_capture_bd_0/fifo_almost_full',1)]
rows=[]
for i,(src,dst,w) in enumerate(wires,1):
 ns=[n for n,pts in signals.items() if src in pts and dst in pts];assert len(ns)==1,(src,dst)
 rows.append([f'W{i:02}',ns[0],src,dst,w])
assert any({'radar_capture_bd_0/M_AXIS_CAPTURE','axis_fifo_capture/S_AXIS'}==pts for pts in interfaces.values())
for port,v in a['ports'].items():
 ns=[(n,pts) for n,pts in signals.items() if port in pts];assert len(ns)==1,(port,ns)
 n,pts=ns[0];inner=next(x for x in pts if x!=port)
 rows.append(['EXTERNAL',n,port if v['direction']=='I' else inner,inner if v['direction']=='I' else port,1])
with (delivery/'connections.tsv').open('w',newline='',encoding='utf-8') as f:
 writer=csv.writer(f,delimiter='\t');writer.writerow(['id','net','driver','sink','width']);writer.writerows(rows)
with (delivery/'ip_parameter_diff.tsv').open('w',encoding='utf-8') as f:
 f.write('cell\tparameter\tbefore\tafter\tresult\n')
 for n,c in b['components'].items():
  if n=='radar_capture_bd_0':continue
  for p,v in c.get('parameters',{}).items():f.write(f"{n}\t{p}\t{v.get('value')}\t{v.get('value')}\tUNCHANGED\n")
x=ET.parse(root/'Radar_XCZU2CG_M2.xpr').getroot()
fs={s.attrib['Name']:s for s in x.findall('./FileSets/FileSet')}
options=lambda s:{v.attrib['Name']:v.attrib.get('Val') for v in s.findall('./Config/Option')}
assert options(fs['sources_1'])['TopModule']=='radar_m2_top'
assert options(fs['sim_1'])['TopModule']=='tb_capture_fifo'
assert 'clk_wiz_sys' not in fs
for f in fs['sources_1'].findall('File'):
 p=f.attrib['Path'];assert '/imports/rtl/' not in p and 'clk_wiz_sys' not in p,p
for f in fs['utils_1'].findall('File'):assert not f.attrib['Path'].endswith('.dcp'),f.attrib
for run in x.findall('./Runs/Run'):
 assert run.attrib.get('AutoIncrementalCheckpoint','false')=='false',run.attrib
 assert not run.attrib.get('IncrementalCheckpoint'),run.attrib
wrapper=(root/'Radar_XCZU2CG_M2.gen/sources_1/bd/radar_system/hdl/radar_system_wrapper.v').read_text()
assert len(re.findall(r'^\s*(?:input|output|inout)\s',wrapper,re.M))==18
assert len(a['ports'])==16 and len(a['interface_ports'])==1
result={'status':'PASS','cells':12,'interfaces':len(interfaces),'signals':len(signals),'protected_parameter_changes':diff,'wiring_checks':len(rows)+1,'wrapper_ports':18,'top':'radar_m2_top','simulation_top':'tb_capture_fifo'}
(delivery/'structure_status.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
print(json.dumps(result,indent=2))
