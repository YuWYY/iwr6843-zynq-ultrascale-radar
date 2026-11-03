# Incremental, offline-only integration. Never recreates the baseline BD.
set root [file normalize [file join [file dirname [info script]] ../..]]
set evidence /software/verification/20261101_pl_dsp_01/checks/native_integration
if {[llength $argv] > 1} {error {Expected optional evidence directory}}
if {[llength $argv] == 1} {set evidence [file normalize [lindex $argv 0]]}
file mkdir $evidence
proc one {objects context} {
    if {[llength $objects] != 1} {error "$context: expected exactly one object, got $objects"}
    return [lindex $objects 0]
}
proc join_interface {a b} {
    set a [one [get_bd_intf_pins -quiet $a] $a]
    set b [one [get_bd_intf_pins -quiet $b] $b]
    set na [get_bd_intf_nets -quiet -of_objects $a]
    set nb [get_bd_intf_nets -quiet -of_objects $b]
    if {$na ne {} && $nb ne {}} {
        if {$na ne $nb} {error "Refusing to replace existing interface connection: $a $b"}
        return
    }
    connect_bd_intf_net $a $b
}
proc join_pin {a b} {
    set a [one [get_bd_pins -quiet $a] $a]
    set b [one [get_bd_pins -quiet $b] $b]
    set na [get_bd_nets -quiet -of_objects $a]
    set nb [get_bd_nets -quiet -of_objects $b]
    if {$na ne {} && $nb ne {}} {
        if {$na ne $nb} {error "Refusing to replace existing scalar connection: $a $b"}
        return
    }
    connect_bd_net $a $b
}
proc config_snapshot {name} {
    set cell [one [get_bd_cells $name] $name]
    set result {}
    foreach p [lsort [list_property $cell]] {
        if {[string match CONFIG.* $p]} {dict set result $p [get_property $p $cell]}
    }
    return $result
}
open_project $root/Radar_XCZU2CG_M2.xpr
set xpm_libs [get_property XPM_LIBRARIES [current_project]]
foreach library {XPM_CDC XPM_MEMORY} {
    if {[lsearch -exact $xpm_libs $library]<0} {lappend xpm_libs $library}
}
set_property XPM_LIBRARIES $xpm_libs [current_project]
if {[get_property PART [current_project]] ne "xczu2cg-sfvc784-1-e"} {error "Wrong device"}
if {[version -short] ne "2025.1"} {error "Requires Vivado 2025.1"}
set bd [one [get_files -quiet */radar_system.bd] radar_system.bd]
open_bd_design $bd
set protected {}
foreach cell {zynq_ultra_ps_e_0 axi_dma_capture axi_gpio_ctrl axis_fifo_capture rst_sys_100m clk_wiz_phy_ref rst_phy_ref_300m const_zero const_one} {
    dict set protected $cell [config_snapshot $cell]
}
set f [open $evidence/protected_before.txt w];puts $f $protected;close $f
write_bd_tcl -force $evidence/before.tcl

foreach source [glob -nocomplain $root/rtl/dsp/*.v $root/rtl/dsp/*.sv] {
    if {![llength [get_files -quiet $source]]} {add_files -norecurse $source}
}
foreach core {radar_fft_range radar_fft_doppler} {
    set source $root/ip/pl_dsp/$core/$core.xci
    if {![file exists $source]} {error "Missing native FFT IP: $source"}
    if {![llength [get_files -quiet $source]]} {add_files -norecurse $source}
    generate_target all [get_files $source]
}
update_compile_order -fileset sources_1
if {![llength [get_bd_cells -quiet radar_dsp_accel_0]]} {
    create_bd_cell -type module -reference radar_dsp_accel radar_dsp_accel_0
} else {
    set module_ip [get_property CONFIG.Component_Name [get_bd_cells radar_dsp_accel_0]]
    update_module_reference [get_ips $module_ip]
}
if {![llength [get_bd_cells -quiet radar_dsp_datamover]]} {
    create_bd_cell -type ip -vlnv xilinx.com:ip:axi_datamover:5.1 radar_dsp_datamover
}
set_property -dict [list CONFIG.c_enable_mm2s {1} CONFIG.c_enable_s2mm {1} CONFIG.c_m_axi_mm2s_data_width {64} CONFIG.c_m_axis_mm2s_tdata_width {64} CONFIG.c_m_axi_s2mm_data_width {64} CONFIG.c_s_axis_s2mm_tdata_width {64} CONFIG.c_addr_width {32} CONFIG.c_mm2s_btt_used {23} CONFIG.c_s2mm_btt_used {23} CONFIG.c_mm2s_burst_size {64} CONFIG.c_s2mm_burst_size {64} CONFIG.c_include_mm2s_dre {false} CONFIG.c_include_s2mm_dre {false}] [get_bd_cells radar_dsp_datamover]
set_property CONFIG.NUM_MI 3 [get_bd_cells smartconnect_ctrl]
set_property -dict [list CONFIG.c_enable_mm2s_adv_sig {1} CONFIG.c_enable_s2mm_adv_sig {1}] [get_bd_cells radar_dsp_datamover]
set_property CONFIG.NUM_SI 4 [get_bd_cells smartconnect_data]
join_interface smartconnect_ctrl/M02_AXI radar_dsp_accel_0/S_AXI
join_interface radar_dsp_datamover/M_AXI_MM2S smartconnect_data/S02_AXI
join_interface radar_dsp_datamover/M_AXI_S2MM smartconnect_data/S03_AXI
foreach {a b} {
    M_AXIS_MM2S_CMD S_AXIS_MM2S_CMD
    S_AXIS_MM2S_STS M_AXIS_MM2S_STS
    S_AXIS_MM2S M_AXIS_MM2S
    M_AXIS_S2MM_CMD S_AXIS_S2MM_CMD
    S_AXIS_S2MM_STS M_AXIS_S2MM_STS
    M_AXIS_S2MM S_AXIS_S2MM
} {join_interface radar_dsp_accel_0/$a radar_dsp_datamover/$b}
join_pin zynq_ultra_ps_e_0/pl_clk0 radar_dsp_accel_0/aclk
join_pin rst_sys_100m/peripheral_aresetn radar_dsp_accel_0/aresetn
foreach pin {m_axi_mm2s_aclk m_axis_mm2s_cmdsts_aclk m_axi_s2mm_aclk m_axis_s2mm_cmdsts_awclk} {
    join_pin zynq_ultra_ps_e_0/pl_clk0 radar_dsp_datamover/$pin
}
foreach pin {m_axi_mm2s_aresetn m_axis_mm2s_cmdsts_aresetn m_axi_s2mm_aresetn m_axis_s2mm_cmdsts_aresetn} {
    join_pin radar_dsp_accel_0/dsp_datamover_aresetn radar_dsp_datamover/$pin
}
foreach signal {mm2s_halt mm2s_halt_cmplt s2mm_halt s2mm_halt_cmplt} {
    join_pin radar_dsp_accel_0/$signal radar_dsp_datamover/$signal
}
foreach signal {mm2s_allow_addr_req s2mm_allow_addr_req} {
    join_pin const_one/dout radar_dsp_datamover/$signal
}
if {![llength [get_bd_cells -quiet dsp_zero4]]} {
    create_bd_cell -type ip -vlnv xilinx.com:ip:xlconstant:1.1 dsp_zero4
}
set_property -dict [list CONFIG.CONST_WIDTH {4} CONFIG.CONST_VAL {0}] [get_bd_cells dsp_zero4]
foreach signal {mm2s_dbg_sel s2mm_dbg_sel} {
    join_pin dsp_zero4/dout radar_dsp_datamover/$signal
}
foreach signal {valid ready last} {
    # A scalar tap overrides the interface-managed member; explicitly preserve
    # BOTH original endpoints as well as the passive observation input.
    join_pin axis_fifo_capture/m_axis_t$signal axi_dma_capture/s_axis_s2mm_t$signal
    join_pin axis_fifo_capture/m_axis_t$signal radar_dsp_accel_0/capture_${signal}_observe
}

set space [one [get_bd_addr_spaces zynq_ultra_ps_e_0/Data] PS_DATA]
set slave [one [get_bd_addr_segs radar_dsp_accel_0/S_AXI/*] DSP_CSR]
assign_bd_address -offset 0xA0020000 -range 0x00010000 -target_address_space $space $slave
foreach port {MM2S S2MM} {
    set space [one [get_bd_addr_spaces radar_dsp_datamover/Data_$port] $port]
    assign_bd_address -offset 0x00000000 -range 0x80000000 -target_address_space $space [get_bd_addr_segs zynq_ultra_ps_e_0/SAXIGP2/HP0_DDR_LOW]
}
validate_bd_design
dict for {name baseline} $protected {
    set after [config_snapshot $name]
    if {$baseline ne $after} {error "Protected configuration changed: $name"}
}
set f [open $evidence/protected_after.txt w];puts $f $protected;close $f
set f [open $evidence/addresses.txt w]
foreach seg [get_bd_addr_segs -hier] {
    puts $f "$seg OFFSET=[get_property OFFSET $seg] RANGE=[get_property RANGE $seg]"
}
close $f
save_bd_design
write_bd_tcl -force $evidence/after.tcl
source [file join [file dirname [info script]] layout.tcl]
generate_target all $bd
set wrapper [make_wrapper -files $bd -top]
foreach source $wrapper {if {![llength [get_files -quiet $source]]} {add_files -norecurse $source}}
set_property TOP radar_m2_top [get_filesets sources_1]
update_compile_order -fileset sources_1
report_ip_status -file $evidence/ip_status.rpt
puts {M2_PL_DSP_INCREMENTAL_INTEGRATION_PASS}
close_project
exit
