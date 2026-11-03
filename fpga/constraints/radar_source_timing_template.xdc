# NOT automatically imported. External source timing remains PENDING.
# Before read_xdc, define all four reviewed variables in full Tcl and call
# m2_validate_source_timing_values from scripts/m2_real/check_constraints.tcl.
# Do not invent zero delays to remove no_input_delay warnings.
# Common intervals must enclose independently reviewed FRCLK/DATA0/DATA1 bounds.
set radar_d [get_ports {radar_frclk_p radar_data0_p radar_data1_p}]
set_input_delay -clock [get_clocks RADAR_BCLK300] -min $RADAR_IN_MIN_R_NS $radar_d
set_input_delay -clock [get_clocks RADAR_BCLK300] -max $RADAR_IN_MAX_R_NS $radar_d
set_input_delay -clock [get_clocks RADAR_BCLK300] -clock_fall -add_delay -min $RADAR_IN_MIN_F_NS $radar_d
set_input_delay -clock [get_clocks RADAR_BCLK300] -clock_fall -add_delay -max $RADAR_IN_MAX_F_NS $radar_d
