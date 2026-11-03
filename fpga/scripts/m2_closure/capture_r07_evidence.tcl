# Read-only report collection from the current mapped/routed design.
# No XDC is applied here. Missing source timing is recorded, not manufactured.
source [file join [file dirname [info script]] native_export.tcl]
proc m2r07_capture {root reports stage} {
    if {$stage ni {synth route}} {error {R07 stage must be synth or route}}
    file mkdir $reports
    set base u_system/radar_system_i/radar_capture_bd_0/inst/u_phy
    set ports {}
    foreach name {radar_bitclk_p radar_bitclk_n radar_frclk_p radar_frclk_n radar_data0_p radar_data0_n radar_data1_p radar_data1_n phy_ref_clk_100m_clk_p phy_ref_clk_100m_clk_n radar_uart_tx radar_uart_rx radar_nerr_n radar_3v3_present radar_sync radar_nrst_n} {
        set p [get_ports -quiet $name]
        if {[llength $p] != 1} {error "R07 port missing/ambiguous: $name"}
        lappend ports "[m2c::json_string $name]:[m2c::json_dict [m2c::props $p {PACKAGE_PIN IOSTANDARD DIFF_TERM DIFF_TERM_ADV DRIVE SLEW DIRECTION}]]"
    }
    set active_source false; set active_template false
    if {[llength [current_project -quiet]]} {
        foreach f [get_files -compile_order constraints -used_in implementation] {
            if {[file tail [get_property NAME $f]] eq "radar_source_timing.xdc"} {set active_source true}
            if {[file tail [get_property NAME $f]] eq "radar_source_timing_template.xdc"} {set active_template true}
        }
    }
    write_xdc -type timing -force [file join $reports r07_effective_timing.xdc]
    report_datasheet -file [file join $reports r07_datasheet.rpt]
    set delay_cells {}; set ibufs {}
    foreach lane {0 1 2} {
        set name [format {%s/g_lane[%d].u_delay} $base $lane]
        set c [get_cells -quiet $name]
        if {[llength $c] != 1} {error "R07 delay primitive missing: $name"}
        lappend delay_cells "[m2c::json_string $name]:[m2c::json_dict [m2c::props $c {REF_NAME DELAY_FORMAT DELAY_TYPE DELAY_VALUE DELAY_SRC REFCLK_FREQUENCY LOC BEL}]]"
    }
    foreach leaf {u_bclk_ibuf u_fr_ibuf u_d0_ibuf u_d1_ibuf} {
        set c [get_cells -quiet $base/$leaf]
        if {[llength $c] != 1} {error "R07 input buffer missing: $leaf"}
        set properties [m2c::props $c {REF_NAME IOSTANDARD DIFF_TERM DIFF_TERM_ADV LOC BEL}]
        foreach pin {I IB} {
            set connected [get_ports -quiet -of_objects [get_nets -quiet -of_objects [get_pins $base/$leaf/$pin]]]
            dict set properties ${pin}_PORT [join $connected ,]
        }
        lappend ibufs "[m2c::json_string $leaf]:[m2c::json_dict $properties]"
    }
    set clk [get_clocks -quiet RADAR_BCLK300]
    if {[llength $clk] != 1} {error {R07 primary BITCLK clock missing/ambiguous}}
    set clocks {}
    foreach name {RADAR_BCLK300 RADAR_SER300 RADAR_RX75} {
        set clock [get_clocks -quiet $name]
        if {[llength $clock] != 1} {error "R07 missing clock: $name"}
        lappend clocks "[m2c::json_string $name]:[m2c::json_dict [m2c::props $clock {PERIOD WAVEFORM IS_GENERATED MASTER_CLOCK SOURCE_PINS DIVIDE_BY MULTIPLY_BY}]]"
    }
    set paths_json {}
    foreach {port leaf} {radar_data0_p u_d0_ser radar_data1_p u_d1_ser radar_frclk_p u_fr_ser} {
        set endpoint [get_pins -quiet $base/$leaf/D]
        if {[llength $endpoint] != 1} {error "R07 capture endpoint missing: $leaf/D"}
        foreach edge {rise fall} {
            foreach bound {min max} {
                # Edge selector applies to the CLOCK object, not a data transition.
                set options [list -${edge}_from $clk -to $endpoint -delay_type $bound -max_paths 1 -nworst 1]
                set paths [get_timing_paths -quiet {*}$options]
                set ignored [get_timing_paths -quiet -user_ignored {*}$options]
                set file [file join $reports r07_${port}_${edge}_${bound}.rpt]
                set worst [dict create]
                if {[llength $paths]} {
                    report_timing {*}$options -path_type full_clock_expanded -input_pins -significant_digits 3 -file $file
                    set worst [m2c::props [lindex $paths 0] {STARTPOINT_PIN ENDPOINT_PIN STARTPOINT_CLOCK ENDPOINT_CLOCK STARTPOINT_CLOCK_EDGE ENDPOINT_CLOCK_EDGE INPUT_DELAY DELAY_TYPE EXCEPTION SLACK REQUIREMENT DATAPATH_DELAY}]
                } else {
                    set f [open $file w]; fconfigure $f -encoding utf-8 -translation lf
                    puts $f "NO_CONSTRAINED_INPUT_PATH: $port reference $edge edge / $bound. No numeric source budget inferred."
                    close $f
                }
                lappend paths_json "\{\"port\":[m2c::json_string $port],\"edge\":[m2c::json_string $edge],\"bound\":[m2c::json_string $bound],\"path_count\":[llength $paths],\"ignored_path_count\":[llength $ignored],\"worst\":[m2c::json_dict $worst]\}"
            }
        }
    }
    set f [open [file join $reports r07_native.json] {WRONLY CREAT EXCL}]
    fconfigure $f -encoding utf-8 -translation lf
    puts $f "\{\"schema\":1,\"tool_version\":[m2c::json_string [version -short]],\"stage\":[m2c::json_string $stage],\"active_source_xdc\":$active_source,\"active_template_xdc\":$active_template,\"ports\":\{[join $ports ,]\},\"delay_cells\":\{[join $delay_cells ,]\},\"ibufs\":\{[join $ibufs ,]\},\"clocks\":\{[join $clocks ,]\},\"paths\":\[[join $paths_json ,]\]\}"
    close $f
}
