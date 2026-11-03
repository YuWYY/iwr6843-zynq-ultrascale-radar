# Layout only: capture chain above, DDR processing below, control/reset separate.
set placements {}
foreach {name level x y} {
    radar_capture_bd_0 1 350 560
    axis_fifo_capture 2 950 560
    axi_dma_capture 3 1550 560
    smartconnect_data 4 2150 800
    zynq_ultra_ps_e_0 5 2800 600
    radar_dsp_accel_0 2 950 1350
    radar_dsp_datamover 3 1550 1350
    dsp_zero4 3 1550 1770
    axi_gpio_ctrl 2 950 80
    smartconnect_ctrl 3 1550 80
    clk_wiz_phy_ref 1 350 2050
    rst_phy_ref_300m 2 950 2050
    const_zero 3 1550 1960
    const_one 3 1550 2110
    rst_sys_100m 4 2150 2050
} {append placements "preplace inst $name -pg 1 -lvl $level -x $x -y $y -defaultsOSRD\n"}
set ypos 220
foreach name {radar_bitclk_p radar_bitclk_n radar_frclk_p radar_frclk_n radar_data0_p radar_data0_n radar_data1_p radar_data1_n radar_uart_rx_raw radar_3v3_raw radar_nerr_n_raw radar_uart_tx_data radar_uart_tx_oe radar_nrst_low radar_sync_data radar_sync_oe} {
    append placements "preplace port $name -pg 1 -lvl 0 -x -30 -y $ypos -defaultsOSRD\n"
    incr ypos 45
}
append placements "preplace port phy_ref_clk_100m -pg 1 -lvl 0 -x -30 -y 2050 -defaultsOSRD\n"
set layout "\"ActiveEmotionalView\":\"Default View\",\n\"guistr\":\"# # String gsaved with Nlview 7.8.0 2024-04-26 e1825d835c VDI=44 GEI=38 GUI=JA:21.0\n# -string -flagsOSRD\n${placements}levelinfo -pg 1 -30 350 950 1550 2150 2800 3200\n\"\n"
regenerate_bd_layout -layout_string $layout
write_bd_layout -force -format native [file join $root Radar_XCZU2CG_M2.srcs sources_1 bd radar_system ui bd_93d232e8.ui]
write_bd_layout -force -format native $evidence/radar_system.ui
start_gui
write_bd_layout -force -format svg -orientation portrait $evidence/radar_system.svg
stop_gui
save_bd_design
