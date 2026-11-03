# F20 XCZU2CG-1SFVC784E + supplied direct-wire adapter netlist.
# REQUIRED physical conditions, not measurements: Bank64=1.2V, AC-coupled,
# bias to ~0.6V, external 100 ohm. Actual board confirmation is PENDING.
# Radar: Bank65 MUST physically be 1.8V. Four input LVDS pairs have no
# termination on the supplied adapter; use internal differential termination.
# Control Bank44=3.3V. PS MIO settings are unrelated and remain LVCMOS18.
# Pure declarative XDC. Assertions are in scripts/m2_real/check_constraints.tcl.
set_property PACKAGE_PIN AE5 [get_ports {phy_ref_clk_100m_clk_p}]
set_property PACKAGE_PIN AF5 [get_ports {phy_ref_clk_100m_clk_n}]
set_property PACKAGE_PIN L7 [get_ports {radar_bitclk_p}]
set_property PACKAGE_PIN L6 [get_ports {radar_bitclk_n}]
set_property PACKAGE_PIN L3 [get_ports {radar_frclk_p}]
set_property PACKAGE_PIN L2 [get_ports {radar_frclk_n}]
set_property PACKAGE_PIN K8 [get_ports {radar_data0_p}]
set_property PACKAGE_PIN K7 [get_ports {radar_data0_n}]
set_property PACKAGE_PIN J6 [get_ports {radar_data1_p}]
set_property PACKAGE_PIN H6 [get_ports {radar_data1_n}]
set_property PACKAGE_PIN AG10 [get_ports {radar_uart_tx}]
set_property PACKAGE_PIN AH10 [get_ports {radar_uart_rx}]
set_property PACKAGE_PIN AE12 [get_ports {radar_nerr_n}]
set_property PACKAGE_PIN AF12 [get_ports {radar_3v3_present}]
set_property PACKAGE_PIN AE10 [get_ports {radar_sync}]
set_property PACKAGE_PIN AF10 [get_ports {radar_nrst_n}]
set_property IOSTANDARD DIFF_SSTL12 [get_ports {phy_ref_clk_100m_clk_p phy_ref_clk_100m_clk_n}]
# DIFF_SSTL12 does not support DIFF_TERM_ADV, including TERM_NONE. Do not
# assign the legacy DIFF_TERM FALSE property: Vivado translates it into an
# illegal explicit TERM_NONE and raises PORTPROP-6. External termination and
# the DIFF_SSTL12 electrical contract remain required and physically PENDING.
set_property IOSTANDARD LVDS [get_ports {radar_bitclk_p radar_bitclk_n radar_frclk_p radar_frclk_n radar_data0_p radar_data0_n radar_data1_p radar_data1_n}]
set_property DIFF_TERM TRUE [get_ports {radar_bitclk_p radar_bitclk_n radar_frclk_p radar_frclk_n radar_data0_p radar_data0_n radar_data1_p radar_data1_n}]
set_property IOSTANDARD LVCMOS33 [get_ports {radar_uart_tx radar_uart_rx radar_nerr_n radar_3v3_present radar_sync radar_nrst_n}]
set_property DRIVE 4 [get_ports {radar_uart_tx radar_sync radar_nrst_n}]
set_property SLEW SLOW [get_ports {radar_uart_tx radar_sync radar_nrst_n}]
set_property PULLDOWN TRUE [get_ports radar_3v3_present]
# No FPGA-side pull-up is added to pins on an unpowered radar.
