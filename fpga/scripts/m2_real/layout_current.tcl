# Run in the native Vivado GUI after integration; only changes the BD layout.
source [file join [file dirname [info script]] common.tcl]
m2real::open
set placements {}
foreach {name level x y} {
    radar_capture_bd_0 1 330 520
    axis_fifo_capture 2 870 520
    axi_dma_capture 3 1300 520
    smartconnect_data 4 1730 520
    zynq_ultra_ps_e_0 5 2190 520
    axi_gpio_ctrl 2 870 80
    smartconnect_ctrl 3 1300 80
    clk_wiz_phy_ref 1 330 1170
    rst_phy_ref_300m 2 870 1170
    const_zero 3 1300 1090
    const_one 3 1300 1230
    rst_sys_100m 4 1730 1170
} {
    append placements "preplace inst $name -pg 1 -lvl $level -x $x -y $y -defaultsOSRD\n"
}
set ypos 220
foreach name {radar_bitclk_p radar_bitclk_n radar_frclk_p radar_frclk_n radar_data0_p radar_data0_n radar_data1_p radar_data1_n radar_uart_rx_raw radar_3v3_raw radar_nerr_n_raw radar_uart_tx_data radar_uart_tx_oe radar_nrst_low radar_sync_data radar_sync_oe} {
    append placements "preplace port $name -pg 1 -lvl 0 -x -30 -y $ypos -defaultsOSRD\n"
    incr ypos 45
}
append placements "preplace port phy_ref_clk_100m -pg 1 -lvl 0 -x -30 -y 1170 -defaultsOSRD\n"
set layout "\"ActiveEmotionalView\":\"Default View\",\n\"guistr\":\"# # String gsaved with Nlview 7.8.0 2024-04-26 e1825d835c VDI=44 GEI=38 GUI=JA:21.0\n# -string -flagsOSRD\n${placements}levelinfo -pg 1 -30 330 870 1300 1730 2190 2550\n\"\n"
regenerate_bd_layout -layout_string $layout
set layoutdir [file join $m2real::delivery layout]
file mkdir $layoutdir
write_bd_layout -force -format native [file join $m2real::root Radar_XCZU2CG_M2.srcs sources_1 bd radar_system ui bd_93d232e8.ui]
write_bd_layout -force -format native [file join $layoutdir radar_system.ui]
write_bd_layout -force -format svg -orientation portrait [file join $layoutdir radar_system.svg]
puts {M2_LAYOUT_EXPORTED}
