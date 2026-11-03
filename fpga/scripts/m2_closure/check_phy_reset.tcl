# Native structural and min/max timing proof for the UG571 reset sequence.
# No constraints/exceptions are created here; empty/ignored paths cannot pass.
proc m2c::reset_sources {endpoint expected clock} {
    set drivers [get_pins -quiet -leaf -of_objects [get_nets -segments -of_objects $endpoint] -filter {DIRECTION == OUT}]
    m2c::require {[llength $drivers] == 1} "Reset/control must have one leaf driver: $endpoint / $drivers"
    set driver_cell [get_cells -of_objects $drivers]
    if {[get_property IS_SEQUENTIAL $driver_cell]} {
        m2c::require {[get_property REF_PIN_NAME $drivers] eq "Q"} {Reset source is not a register Q}
        set starts $drivers
    } else {
        set ref_name [get_property REF_NAME $driver_cell]
        m2c::require {$ref_name in {INV LUT1}} "Reset path has logic other than polarity inversion: $driver_cell"
        if {$ref_name eq "LUT1"} {m2c::require {[get_property INIT $driver_cell] in {2'h1 2'b01}} {Reset LUT1 is not an inverter}}
        set starts [all_fanin -flat -startpoints_only -to $drivers]
    }
    m2c::require {[llength $starts] > 0} "Reset/control has no registered source: $endpoint"
    set names {}
    foreach pin $starts {
        set cell [get_cells -quiet -of_objects $pin]
        m2c::require {[llength $cell] == 1 && [get_property IS_SEQUENTIAL $cell]} "Unregistered reset/control source: $pin"
        set name [get_property NAME $cell]
        m2c::require {[string match $expected $name]} "Wrong reset/control source: $name expected $expected"
        set actual [get_clocks -quiet -of_objects [get_pins -of_objects $cell -filter {REF_PIN_NAME == C}]]
        m2c::require {$actual eq $clock} "Wrong reset/control source clock: $name / $actual"
        lappend names $name
    }
    return [lsort -unique $names]
}
proc m2c::reset_path_pair {reports label kind endpoint sources bound} {
    variable verification_stage
    set cell [get_cells -of_objects $endpoint]
    set ref_name [get_property REF_NAME $cell]
    set arcs [get_timing_arcs -of_objects $cell]
    m2c::require {[llength $arcs] > 0} "Missing endpoint library model: $endpoint"
    set checks {}; set reset_arcs {}; set arc_rows {}
    foreach arc $arcs {
        lappend arc_rows [m2c::json_dict [m2c::props $arc [list_property $arc]]]
        set type [string tolower [get_property TYPE $arc]]
        if {[get_property TO_PIN $arc] eq [get_property NAME $endpoint] && $type in {recovery removal setup hold}} {
            m2c::require {![get_property IS_USER_DISABLED $arc] && ![get_property IS_DISABLED $arc]} "Reset native check disabled: $arc"
            lappend checks $type
        }
        if {[get_property FROM_PIN $arc] eq [get_property NAME $endpoint] && $type eq "reg set/clr"} {
            m2c::require {![get_property IS_USER_DISABLED $arc]} "Reset propagation arc user-disabled: $arc"
            lappend reset_arcs $arc
        }
    }
    if {$kind eq "PRIMITIVE_RST"} {
        m2c::require {$ref_name in {IDELAYE3 ISERDESE3} && [llength $checks] == 0 && [llength $reset_arcs] > 0} {Primitive no-native-check library contract changed}
    } else {
        m2c::require {"recovery" in $checks && "removal" in $checks} "Required native R/R library arcs missing: $endpoint"
    }
    set af [open [file join $reports reset_${label}_library.json] {WRONLY CREAT EXCL}]
    puts $af "\[[join $arc_rows ,]\]"; close $af
    set records {}
    foreach mode {max min} {
        set paths [get_timing_paths -from [get_cells $sources] -to $endpoint -delay_type $mode -max_paths 1 -nworst 1]
        m2c::require {[llength $paths] == 1} "Reset $mode path absent/excepted: $endpoint"
        set raw_slack [get_property SLACK $paths]
        set raw_requirement [get_property REQUIREMENT $paths]
        set native_check NATIVE
        if {$kind eq "PRIMITIVE_RST" && $mode eq "min"} {
            m2c::require {$raw_slack eq "" && $raw_requirement eq ""} {Unexpected primitive min check; review changed library instead of waiving it}
            set native_check NO_LIBRARY_CHECK; set slack null; set requirement null
        } else {
            set slack [m2c::finite $raw_slack "$label $mode slack"]
            set requirement [m2c::finite $raw_requirement "$label $mode requirement"]
            if {$verification_stage eq "route"} {m2c::require {$slack >= 0} "Reset $mode timing violation: $endpoint slack=$slack"}
        }
        set data [m2c::finite [get_property DATAPATH_DELAY $paths] "$label $mode datapath"]
        set clock_delay [m2c::finite [get_property ENDPOINT_CLOCK_DELAY $paths] "$label clock propagation"]
        if {$kind eq "PRIMITIVE_RST" && $mode eq "max"} {m2c::require {abs($requirement-$bound)<.001} {Primitive RST physical max bound changed}}
        m2c::require {$data >= 0 && $data <= $bound} "Reset physical delay exceeds $bound ns: $endpoint / $data"
        set start [get_property STARTPOINT_PIN $paths]
        set end [get_property ENDPOINT_PIN $paths]
        m2c::require {$end eq [get_property NAME $endpoint]} "Reset endpoint was substituted: $end"
        set exception [get_property EXCEPTION $paths]
        m2c::require {![regexp -nocase {false|datapath.only} $exception]} "Reset R/R hidden by exception: $exception"
        report_timing -of_objects $paths -file [file join $reports reset_${label}_${mode}.rpt]
        lappend records "\{\"delay_type\":[m2c::json_string $mode],\"native_check\":[m2c::json_string $native_check],\"raw_slack\":[m2c::json_string $raw_slack],\"raw_requirement\":[m2c::json_string $raw_requirement],\"startpoint\":[m2c::json_string $start],\"endpoint\":[m2c::json_string $end],\"slack_ns\":$slack,\"datapath_ns\":$data,\"requirement_ns\":$requirement,\"endpoint_clock_delay_ns\":$clock_delay,\"exception\":[m2c::json_string $exception]\}"
    }
    return "\{\"label\":[m2c::json_string $label],\"kind\":[m2c::json_string $kind],\"endpoint\":[m2c::json_string $endpoint],\"primitive_ref\":[m2c::json_string $ref_name],\"native_library_checks\":[m2c::json_array $checks],\"reset_propagation_arcs\":[llength $reset_arcs],\"source_cells\":[m2c::json_array $sources],\"bound_ns\":$bound,\"paths\":\[[join $records ,]\]\}"
}
proc m2c::check_phy_reset {base clocks contract reports} {
    variable verification_stage
    set phy_reset_clock_delay 0.0
    set phy $base/u_phy
    set bclk [dict get $clocks BITCLK]; set rx [dict get $clocks RX]; set ref [dict get $clocks REF]
    set ser_cell [get_cells -quiet $phy/u_serdes_div]
    m2c::require {[llength $ser_cell] == 1 && [get_property REF_NAME $ser_cell] eq "BUFGCE_DIV" && [get_property BUFGCE_DIVIDE $ser_cell] == 1} {ISERDES needs dedicated /1 BUFGCE_DIV}
    set ser [m2c::one_clock [get_pins $phy/u_serdes_div/O] [get_property PERIOD $bclk] SER300]
    m2c::require {[get_property IS_GENERATED $ser] && [get_property MASTER_CLOCK $ser] eq [get_property NAME $bclk]} {SER300 must be generated from BITCLK}
    foreach pin {I CLR CE} {
        set a [get_nets -quiet -of_objects [get_pins $phy/u_serdes_div/$pin]]
        set b [get_nets -quiet -of_objects [get_pins $phy/u_rx_div/$pin]]
        m2c::require {[llength $a] == 1 && $a eq $b} "PHY /1 and /4 must share $pin"
    }
    foreach inst {u_fr_ser u_d0_ser u_d1_ser} {
        set cell [get_cells $phy/$inst]
        m2c::require {[get_property IS_CLK_INVERTED $cell] in {0 1'b0} && [get_property IS_CLK_B_INVERTED $cell] in {1 1'b1}} {ISERDES internal CLK_B inversion mismatch}
        foreach pin {CLK CLK_B} {
            set n [get_nets -of_objects [get_pins $phy/$inst/$pin]]
            set driver [get_nets -of_objects [get_pins $phy/u_serdes_div/O]]
            m2c::require {[llength $n] == 1 && $n eq $driver} "ISERDES $pin bypasses gated /1 clock"
            m2c::require {[get_clocks -of_objects [get_pins $phy/$inst/$pin]] eq $ser} {Wrong ISERDES serial clock}
        }
    }
    set enable [get_pins -quiet $phy/u_bitclk_release/g_async.u_reset_sync/src_arst]
    m2c::require {[llength $enable] == 1} {Missing XPM gate permission boundary}
    m2c::reset_sources $enable $phy/u_cal_ctrl/clock_enable_ref_reg* $ref
    set bound [m2c::finite [dict get $contract primitive_reset_bound_ns] reset_bound]
    m2c::require {$bound == 8.0 && [dict get $contract primitive_before_ctrl_ref_cycles] == 16} {Reset physical/sequence budget not reviewed}
    set margin [expr {16.0*[m2c::finite [get_property PERIOD $ref] REF]-$bound}]
    m2c::require {$margin > 40.0} {Primitive-reset to controller-release propagation margin too small}
    set result {}; set clear_arc_rows {}; set worst_clear 0.0
    foreach divider {u_serdes_div u_rx_div} {
        # Capture the actual device-library arcs for the clock-stop guard audit.
        # A missing CLR->O propagation number must never be invented as 0ns.
        set arcs [get_timing_arcs -of_objects [get_cells $phy/$divider]]
        m2c::require {[llength $arcs] > 0} "No native clock-buffer timing arcs: $divider"
        set af [open [file join $reports ${divider}_arc_properties.json] {WRONLY CREAT EXCL}]
        fconfigure $af -encoding utf-8 -translation lf
        set arc_rows {}
        foreach arc $arcs {lappend arc_rows [m2c::json_dict [m2c::props $arc [list_property $arc]]]}
        puts $af "\[[join $arc_rows ,]\]"; close $af
        set clear_arcs [get_timing_arcs -from [get_pins $phy/$divider/CLR] -to [get_pins $phy/$divider/O]]
        m2c::require {[llength $clear_arcs] == 1} "Need actual CLR-to-O library arc: $divider"
        m2c::require {![get_property IS_USER_DISABLED $clear_arcs] && [get_property TYPE $clear_arcs] eq "Reg Set/Clr"} "Wrong/user-disabled clear arc: $divider"
        set clear_max [m2c::finite [get_property DELAY_MAX_FALL $clear_arcs] "$divider CLR-to-O max"]
        m2c::require {$clear_max > 0} "Missing/nonphysical CLR-to-O delay: $divider"
        foreach property {DELAY_FAST_MAX_FALL DELAY_SLOW_MAX_FALL} {
            set value [m2c::finite [get_property $property $clear_arcs] "$divider $property"]
            m2c::require {$value >= 0 && $value <= $clear_max+.001} {Clear arc aggregate does not enclose speed corners}
        }
        if {$clear_max > $worst_clear} {set worst_clear $clear_max}
        lappend clear_arc_rows "\{\"buffer\":[m2c::json_string $phy/$divider],\"max_fall_ns\":$clear_max,\"library\":[m2c::json_string [get_property LIB_CELL $clear_arcs]]\}"
        set endpoint [get_pins $phy/$divider/CLR]
        set sources [m2c::reset_sources $endpoint $phy/u_bitclk_release/released_reg* $bclk]
        lappend result [m2c::reset_path_pair $reports $divider DIVIDER_CLR $endpoint $sources 8.0]
    }
    foreach {inst label} {g_lane[0].u_delay delay0 g_lane[1].u_delay delay1 g_lane[2].u_delay delay2 u_fr_ser ser_fr u_d0_ser ser_d0 u_d1_ser ser_d1} {
        set endpoint [get_pins -quiet $phy/$inst/RST]
        m2c::require {[llength $endpoint] == 1} "Missing primitive RST: $inst"
        set sources [m2c::reset_sources $endpoint $phy/u_cal_ctrl/primitive_reset_reg* $ref]
        m2c::require {[llength [get_pins -quiet -of_objects [get_cells $sources] -filter {REF_PIN_NAME == CLR || REF_PIN_NAME == PRE}]] == 0} {Primitive RST can assert outside the ordered REF sequence}
        lappend result [m2c::reset_path_pair $reports $label PRIMITIVE_RST $endpoint $sources $bound]
    }
    set endpoint [get_pins $phy/u_idelayctrl/RST]
    set sources [m2c::reset_sources $endpoint $phy/u_cal_ctrl/idelay_reset_reg* $ref]
    lappend result [m2c::reset_path_pair $reports idelayctrl CONTROLLER_RST $endpoint $sources 8.0]
    # Actual buffer-output-to-each-primitive clock net delay, in ps -> ns.
    # Primitive RST has no endpoint clock; its ENDPOINT_CLOCK_DELAY=0 is not a clock-tree measurement.
    set clock_rows {}; set phy_reset_clock_delay 0.0
    foreach {inst pins} {g_lane[0].u_delay CLK g_lane[1].u_delay CLK g_lane[2].u_delay CLK u_fr_ser {CLK CLK_B CLKDIV} u_d0_ser {CLK CLK_B CLKDIV} u_d1_ser {CLK CLK_B CLKDIV}} {
        foreach name $pins {
            set pin [get_pins $phy/$inst/$name]
            set delays [get_net_delays -of_objects [get_nets -of_objects $pin] -to $pin]
            m2c::require {[llength $delays] == 1} "Missing clock-net delay: $pin"
            set fast [m2c::finite [get_property FAST_MAX $delays] clock_fast_ps]
            set slow [m2c::finite [get_property SLOW_MAX $delays] clock_slow_ps]
            m2c::require {$fast >= 0 && $slow >= 0} {Negative clock-net delay}
            set estimate [get_property ESTIMATED $delays]
            if {$verification_stage eq "route"} {m2c::require {!$estimate} "Clock-net delay still estimated after route: $pin"}
            set value [expr {max($fast,$slow)/1000.0}]
            if {$value > $phy_reset_clock_delay} {set phy_reset_clock_delay $value}
            lappend clock_rows "\{\"pin\":[m2c::json_string $pin],\"max_ns\":$value,\"estimated\":$estimate\}"
        }
    }
    # Ignore the additional three-stage REF feedback latency: only the explicit
    # 8-cycle QUIESCE guard is credited against source->CLR + clear arc + tree.
    set close_guard [expr {[dict get $contract gate_closed_before_reset_ref_cycles]*[m2c::finite [get_property PERIOD $ref] REF]}]
    set close_margin [expr {$close_guard-8.0-$worst_clear-$phy_reset_clock_delay}]
    m2c::require {$close_margin > 0} {Clock-buffer clear propagation exceeds REF quiesce guard}
    set status [expr {$verification_stage eq "route" ? "PASS" : "PRE_ROUTE"}]
    return "\{\"status\":[m2c::json_string $status],\"shared_clock_gate\":\"PASS\",\"clock_gate_clear_arc_audit\":\"PASS\",\"clock_clear_arcs\":\[[join $clear_arc_rows ,]\],\"clock_leaf_delays\":\[[join $clock_rows ,]\],\"clock_tree_max_ns\":$phy_reset_clock_delay,\"clock_close_guard_ns\":$close_guard,\"clock_close_margin_ns\":$close_margin,\"primitive_to_controller_guard_ns\":[expr {$margin+$bound}],\"primitive_reset_bound_ns\":$bound,\"guard_after_propagation_ns\":$margin,\"endpoints\":\[[join $result ,]\]\}"
}
