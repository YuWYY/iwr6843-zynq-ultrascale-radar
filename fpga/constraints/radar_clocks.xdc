# Pure declarative XDC: Vivado 2025.1 read_xdc does not execute Tcl control flow.
# Supported point: 600Mb/s/lane, BITCLK300 DDR, real BUFGCE_DIV /4 -> RX75.
# No -add: each target has one clock definition; the explicit generated clock
# supplies the actual divider relation instead of a second independent clock.
# REF100 is declared by the native Clocking Wizard board XDC; do not override it.
create_clock -name RADAR_BCLK300 -period 3.333333 -waveform {0.000000 1.666667} [get_ports radar_bitclk_p]
create_generated_clock -name RADAR_RX75 -source [get_ports radar_bitclk_p] -divide_by 4 [get_pins u_system/radar_system_i/radar_capture_bd_0/inst/u_phy/u_rx_div/O]
create_generated_clock -name RADAR_SER300 -source [get_ports radar_bitclk_p] -divide_by 1 [get_pins u_system/radar_system_i/radar_capture_bd_0/inst/u_phy/u_serdes_div/O]
# Clocking Wizard outputs remain automatically derived. The full Tcl audit
# requires one correctly related divider clock and the expected periods.
# No asynchronous clock groups or CLOCK_DEDICATED_ROUTE overrides.
