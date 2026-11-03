# Pure declarative NETLIST-ONLY XDC: USED_IN_SYNTHESIS=false, LATE.
# Every object/clock/endpoint is checked by scripts/m2_closure/check_constraints.tcl.
# Three synchronizer stages plus capture: earliest 3*Tdst, fastest REF300=10ns.
# The 8ns bound reserves 2ns nominal margin; RX75 allows >=40ns.
# Exact held buses remain timed. No broad asynchronous clock groups.

set radar_cdc_src_0 [get_cells -hier -include_replicated_objects * -filter {NAME =~ u_system/radar_system_i/radar_capture_bd_0/inst/u_cmd_mb/src_hold_reg* && IS_SEQUENTIAL == 1}]
set radar_cdc_dst_0 [get_cells -hier -include_replicated_objects * -filter {NAME =~ u_system/radar_system_i/radar_capture_bd_0/inst/u_cmd_mb/dst_data_reg* && IS_SEQUENTIAL == 1}]
set_max_delay -datapath_only 8.000 -from $radar_cdc_src_0 -to $radar_cdc_dst_0
set_bus_skew 8.000 -from $radar_cdc_src_0 -to $radar_cdc_dst_0

set radar_cdc_src_1 [get_cells -hier -include_replicated_objects * -filter {NAME =~ u_system/radar_system_i/radar_capture_bd_0/inst/u_snapshot/rx_hold_reg* && IS_SEQUENTIAL == 1}]
set radar_cdc_dst_1 [get_cells -hier -include_replicated_objects * -filter {NAME =~ u_system/radar_system_i/radar_capture_bd_0/inst/u_snapshot/data_reg* && IS_SEQUENTIAL == 1}]
set_max_delay -datapath_only 8.000 -from $radar_cdc_src_1 -to $radar_cdc_dst_1
set_bus_skew 8.000 -from $radar_cdc_src_1 -to $radar_cdc_dst_1

set radar_cdc_src_2 [get_cells -hier -include_replicated_objects * -filter {NAME =~ u_system/radar_system_i/radar_capture_bd_0/inst/u_tap_snapshot/rx_hold_reg* && IS_SEQUENTIAL == 1}]
set radar_cdc_dst_2 [get_cells -hier -include_replicated_objects * -filter {NAME =~ u_system/radar_system_i/radar_capture_bd_0/inst/u_tap_snapshot/data_reg* && IS_SEQUENTIAL == 1}]
set_max_delay -datapath_only 8.000 -from $radar_cdc_src_2 -to $radar_cdc_dst_2
set_bus_skew 8.000 -from $radar_cdc_src_2 -to $radar_cdc_dst_2

set radar_cdc_src_3 [get_cells -hier -include_replicated_objects * -filter {NAME =~ u_system/radar_system_i/radar_capture_bd_0/inst/u_phy/u_delay_mb/src_hold_reg* && IS_SEQUENTIAL == 1}]
set radar_cdc_dst_3 [get_cells -hier -include_replicated_objects * -filter {NAME =~ u_system/radar_system_i/radar_capture_bd_0/inst/u_phy/u_delay_mb/dst_data_reg* && IS_SEQUENTIAL == 1}]
set_max_delay -datapath_only 8.000 -from $radar_cdc_src_3 -to $radar_cdc_dst_3
set_bus_skew 8.000 -from $radar_cdc_src_3 -to $radar_cdc_dst_3

set radar_cdc_src_4 [get_cells -hier -include_replicated_objects * -filter {NAME =~ u_system/radar_system_i/radar_capture_bd_0/inst/u_phy/u_delay_report_mb/src_hold_reg* && IS_SEQUENTIAL == 1}]
set radar_cdc_dst_4 [get_cells -hier -include_replicated_objects * -filter {NAME =~ u_system/radar_system_i/radar_capture_bd_0/inst/u_phy/u_delay_report_mb/dst_data_reg* && IS_SEQUENTIAL == 1}]
set_max_delay -datapath_only 8.000 -from $radar_cdc_src_4 -to $radar_cdc_dst_4
set_bus_skew 8.000 -from $radar_cdc_src_4 -to $radar_cdc_dst_4

set radar_cdc_src_5 [get_cells -hier -include_replicated_objects * -filter {NAME =~ u_system/radar_system_i/radar_capture_bd_0/inst/u_phy/u_cal_report_mb/src_hold_reg* && IS_SEQUENTIAL == 1}]
set radar_cdc_dst_5 [get_cells -hier -include_replicated_objects * -filter {NAME =~ u_system/radar_system_i/radar_capture_bd_0/inst/u_phy/u_cal_report_mb/dst_data_reg* && IS_SEQUENTIAL == 1}]
set_max_delay -datapath_only 8.000 -from $radar_cdc_src_5 -to $radar_cdc_dst_5

# RECAL is event-only; cal_report scalar has max-delay but no inter-bit skew.
# Only explicit synchronizer first-stage D pins are excepted.
set_false_path -to [get_pins -hier -regexp {^u_system/radar_system_i/radar_capture_bd_0/inst/(u_cmd_mb|u_snapshot|u_tap_snapshot|u_phy/(u_delay_mb|u_delay_report_mb|u_recal_mb|u_cal_report_mb))/(req_ff|ack_ff)_reg\[0\](_replica[^/]*)?/D$}]
set_false_path -to [get_pins -hier -regexp {^u_system/radar_system_i/radar_capture_bd_0/inst/(u_complete|u_fault|u_first|u_pending|u_idle|u_fifo_ready|u_deser_ready|u_heartbeat|u_receive/u_abort|u_io/(u_pg|u_nerr|u_uart_rx)|u_phy/(u_cal_ready_rx|u_ready_sync|u_ref_ready_sync|u_rx_exec_sync|u_cal_exec_sync|u_activity_sync|u_gate_open_ref))/sync_ff_reg\[0\](_replica[^/]*)?/D$}]
# XPM reset has vendor-scoped assertion exceptions. No user raw CLR/PRE false path.
# Later synchronization stages and local released_reg recovery/removal stay timed.
set_false_path -to [get_ports {radar_uart_tx radar_nrst_n radar_sync}]
# PS UART1 RX remains under native vendor timing.

# REF first releases the six primitive resets, waits 16 REF periods, then
# releases IDELAYCTRL; clocks remain gated through RDY + 16 REF periods.
# Limit the specific primitive reset propagation without suppressing removal.
# Native checker also requires actual DATAPATH_DELAY <= 8ns and both max/min.
set radar_primitive_reset_src [get_cells -hier -include_replicated_objects * -filter {NAME =~ u_system/radar_system_i/radar_capture_bd_0/inst/u_phy/u_cal_ctrl/primitive_reset_reg* && IS_SEQUENTIAL == 1}]
set radar_primitive_reset_dst [get_pins -hier -regexp {^u_system/radar_system_i/radar_capture_bd_0/inst/u_phy/(g_lane\[[012]\]\.u_delay|u_fr_ser|u_d0_ser|u_d1_ser)/RST$}]
set_max_delay 8.000 -from $radar_primitive_reset_src -to $radar_primitive_reset_dst
