# Full Tcl audit, not XDC. No design mutations or broad exception application.
source [file join [file dirname [info script]] native_export.tcl]
source [file join [file dirname [info script]] check_phy_reset.tcl]
source [file join [file dirname [info script]] check_shared_reset.tcl]
proc m2c::finite {value label} {
    if {![string is double -strict $value] || [regexp -nocase {nan|inf} $value]} {error "$label is missing/non-finite: $value"}
    return [expr {double($value)}]
}
proc m2c::require {condition message} {if {![uplevel 1 [list expr $condition]]} {error $message}}
proc m2c::one_clock {pins expected label} {
    set clocks [get_clocks -quiet -of_objects $pins]
    m2c::require {[llength $clocks] == 1} "$label needs one clock, got $clocks"
    set period [m2c::finite [get_property PERIOD $clocks] "$label period"]
    m2c::require {abs($period-$expected) <= 0.001} "$label wrong period $period"
    return $clocks
}
proc m2c::bit_set {cells stem} {
    set bits {}
    foreach c $cells {
        set name [get_property NAME $c]
        set tail [string range $name [expr {[string last / $name]+1}] end]
        set pattern [format {^%s(?:\[([0-9]+)\])?(?:_replica[^/]*)?$} $stem]
        if {![regexp $pattern $tail -> bit]} {error "Unexpected register name: $name"}
        if {$bit eq ""} {set bit 0}
        lappend bits $bit
    }
    return [lsort -integer -unique $bits]
}
proc m2c::int_array {values} {return "\[[join $values ,]\]"}
proc m2c::clock_role {role clocks} {
    if {![dict exists $clocks $role]} {error "Unknown contract clock role $role"}
    return [dict get $clocks $role]
}
proc m2c_check_constraints {reports contract_file {stage route}} {
    m2c::require {$stage in {synth route}} {Unknown physical verification stage}
    set ::m2c::verification_stage $stage
    source $contract_file
    m2c::require {[dict get $::m2c_contract status] eq "REVIEWED"} {CDC topology contract is not frozen/reviewed}
    set base u_system/radar_system_i/radar_capture_bd_0/inst
    m2c::require {[llength [get_cells -hier -quiet -include_replicated_objects * -filter {IS_PRIMITIVE == 1 && REF_NAME =~ FD*}]] > 0} {Mapped netlist required}
    foreach {name ball standard} {
        phy_ref_clk_100m_clk_p AE5 DIFF_SSTL12 phy_ref_clk_100m_clk_n AF5 DIFF_SSTL12
        radar_bitclk_p L7 LVDS radar_bitclk_n L6 LVDS radar_frclk_p L3 LVDS radar_frclk_n L2 LVDS
        radar_data0_p K8 LVDS radar_data0_n K7 LVDS radar_data1_p J6 LVDS radar_data1_n H6 LVDS
        radar_uart_tx AG10 LVCMOS33 radar_uart_rx AH10 LVCMOS33 radar_nerr_n AE12 LVCMOS33 radar_3v3_present AF12 LVCMOS33 radar_sync AE10 LVCMOS33 radar_nrst_n AF10 LVCMOS33
    } {
        set p [get_ports -quiet $name]
        m2c::require {[llength $p] == 1 && [get_property PACKAGE_PIN $p] eq $ball && [get_property IOSTANDARD $p] eq $standard} "Fixed I/O mismatch: $name"
        if {$standard eq "LVDS"} {m2c::require {[get_property DIFF_TERM $p]} "LVDS termination missing: $name"}
        if {$standard eq "DIFF_SSTL12"} {m2c::require {![get_property DIFF_TERM $p]} {Reference internal termination must be off}}
    }
    foreach p [get_ports {radar_uart_tx radar_nrst_n radar_sync}] {m2c::require {[get_property DRIVE $p] == 4 && [get_property SLEW $p] eq "SLOW"} "Output electrical mismatch: $p"}
    m2c::require {[get_property PULLDOWN [get_ports radar_3v3_present]]} {PG pull-down missing}
    set bclk [m2c::one_clock [get_ports radar_bitclk_p] 3.333333 BITCLK]
    set ref100 [m2c::one_clock [get_ports phy_ref_clk_100m_clk_p] 10.0 REF100]
    set rx_period [expr {4.0*[m2c::finite [get_property PERIOD $bclk] BITCLK]}]
    set divider [get_cells -quiet $base/u_phy/u_rx_div]
    m2c::require {[llength $divider] == 1 && [get_property REF_NAME $divider] eq "BUFGCE_DIV" && [get_property BUFGCE_DIVIDE $divider] == 4} {Wrong RX divider}
    set rx [m2c::one_clock [get_pins $base/u_phy/u_rx_div/O] $rx_period RX]
    m2c::require {[get_property IS_GENERATED $rx] && [get_property MASTER_CLOCK $rx] eq [get_property NAME $bclk]} {RX clock master/divide mismatch}
    foreach {clock half} [list $bclk 1.666667 $ref100 5.0] {
        set waveform [get_property WAVEFORM $clock]
        m2c::require {[llength $waveform] == 2} {Malformed primary clock waveform}
        set a [m2c::finite [lindex $waveform 0] waveform]; set b [m2c::finite [lindex $waveform 1] waveform]
        m2c::require {abs($a)<=.001 && abs($b-$half)<=.001} {Unexpected primary clock waveform}
    }
    set ref [m2c::one_clock [get_pins $base/u_phy/u_idelayctrl/REFCLK] 3.333333 REF300]
    set sys [get_clocks -quiet clk_pl_0]
    m2c::require {[llength $sys] == 1} {Missing native PS SYS clock}
    set sys [m2c::one_clock [get_pins -hier -filter "NAME =~ ${base}/u_control/*/C"] 10.0 SYS]
    set clocks [dict create SYS $sys RX $rx REF $ref BITCLK $bclk]
    set phy_pairs 0
    foreach {lane ser} {0 u_fr_ser 1 u_d0_ser 2 u_d1_ser} {
        set delay_clk [get_pins -quiet [format {%s/u_phy/g_lane[%d].u_delay/CLK} $base $lane]]
        set div_clk [get_pins -quiet $base/u_phy/$ser/CLKDIV]
        m2c::require {[llength $delay_clk] == 1 && [llength $div_clk] == 1} {Missing PHY clock pin}
        set a [get_nets -quiet -of_objects $delay_clk]; set b [get_nets -quiet -of_objects $div_clk]
        m2c::require {[llength $a] == 1 && $a eq $b} "PHY CLK/CLKDIV different net: lane $lane"
        m2c::require {[get_clocks -quiet -of_objects $delay_clk] eq $rx} {PHY control clock not RX}
        incr phy_pairs
    }
    set json_payloads {}; set json_events {}; set expected_parents {}; set total 0
    foreach channel [dict get $::m2c_contract channels] {
        set leaf [dict get $channel instance]; set inst $base/$leaf
        m2c::require {[llength [get_cells -quiet $inst]] == 1} "Missing CDC parent $inst"
        lappend expected_parents $inst
        foreach stem {req_ff ack_ff} {
            set regs [get_cells -hier -quiet -include_replicated_objects * -filter "NAME =~ ${inst}/${stem}_reg* && IS_SEQUENTIAL == 1"]
            m2c::require {[m2c::bit_set $regs ${stem}_reg] eq {0 1 2}} "Wrong req/ack depth: $inst/$stem"
            foreach reg $regs {m2c::require {[get_property ASYNC_REG $reg]} "Missing ASYNC_REG: $reg"}
        }
        if {[dict get $channel kind] eq "event_only"} {
            m2c::require {[string length [dict get $channel constant_reason]] >= 20} {Event-only channel has no constant payload proof}
            set unused_dst [get_cells -hier -quiet -include_replicated_objects * -filter "NAME =~ ${inst}/dst_data_reg* && IS_SEQUENTIAL == 1"]
            m2c::require {[llength $unused_dst] == 0} "Event-only channel unexpectedly has payload destinations: $inst"
            set unused_src [get_cells -hier -quiet -include_replicated_objects * -filter "NAME =~ ${inst}/src_hold_reg* && IS_SEQUENTIAL == 1"]
            foreach reg $unused_src {
                set consumers [all_fanout -flat -endpoints_only -from [get_pins -of_objects $reg -filter {REF_PIN_NAME == Q}]]
                m2c::require {[llength $consumers] == 0} "Event-only payload gained functional consumers: $inst / $consumers"
            }
            lappend json_events "\{\"instance\":[m2c::json_string $inst],\"payload_destination_registers\":0,\"retained_unused_source_registers\":[llength $unused_src],\"functional_payload_consumers\":0\}"
            continue
        }
        set src_stem [dict get $channel source_stem]; set dst_stem [dict get $channel destination_stem]
        set src [get_cells -hier -quiet -include_replicated_objects * -filter "NAME =~ ${inst}/${src_stem}* && IS_SEQUENTIAL == 1"]
        set dst [get_cells -hier -quiet -include_replicated_objects * -filter "NAME =~ ${inst}/${dst_stem}* && IS_SEQUENTIAL == 1"]
        set sbits [m2c::bit_set $src $src_stem]; set dbits [m2c::bit_set $dst $dst_stem]
        set sexpected [lsort -integer [dict get $channel source_bits]]; set dexpected [lsort -integer [dict get $channel destination_bits]]
        set optional {}; if {[dict exists $channel optional_unused_source_bits]} {set optional [dict get $channel optional_unused_source_bits]}
        set required {}; foreach bit $sexpected {if {$bit ni $optional} {lappend required $bit}}
        foreach bit $required {m2c::require {$bit in $sbits} "Missing active source bit $leaf/$bit"}
        foreach bit $sbits {m2c::require {$bit in $sexpected} "Undeclared source bit $leaf/$bit"}
        foreach reg $src {
            set bit [lindex [m2c::bit_set [list $reg] $src_stem] 0]
            if {$bit in $optional} {
                set consumers [all_fanout -flat -endpoints_only -from [get_pins -of_objects $reg -filter {REF_PIN_NAME == Q}]]
                m2c::require {[llength $consumers] == 0} "Optional source bit gained consumer: $reg / $consumers"
            }
        }
        m2c::require {[llength $sbits] && [llength $dbits] && $dbits eq $dexpected} "Payload bit-set mismatch $leaf: actual_src={$sbits} allowed_src={$sexpected} actual_dst={$dbits} expected_dst={$dexpected}"
        set sexpected $sbits
        foreach {objects role} [list $src [dict get $channel source_clock] $dst [dict get $channel destination_clock]] {
            set clk [get_clocks -quiet -of_objects [get_pins -quiet -of_objects $objects -filter {REF_PIN_NAME == C}]]
            m2c::require {$clk eq [m2c::clock_role $role $clocks]} "Wrong payload clock at $leaf ($role): $clk"
        }
        set bound [m2c::finite [dict get $channel bound_ns] bound]
        m2c::require {$bound > 0} {Nonpositive max-delay bound}
        set skew_required [dict get $channel bus_skew_required]
        m2c::require {$skew_required in {0 1} && ($skew_required || ([llength $sbits] == 1 && [llength $dbits] == 1))} {Only a one-bit payload may omit inter-bit skew}
        set endpoints [get_pins -quiet -of_objects $dst -filter {REF_PIN_NAME == D}]
        m2c::require {[llength $endpoints] == [llength $dst]} {Destination D population mismatch}
        set checked 0; set worst 1e30
        foreach endpoint $endpoints {
            set paths [get_timing_paths -from $src -to $endpoint -delay_type max -max_paths 1 -nworst 1]
            m2c::require {[llength $paths] == 1} "Missing/excepted payload path: $endpoint"
            set requirement [m2c::finite [get_property REQUIREMENT $paths] requirement]
            set slack [m2c::finite [get_property SLACK $paths] slack]
            m2c::require {abs($requirement-$bound)<=.001} "Wrong effective requirement $endpoint: $requirement"
            m2c::require {$slack >= 0} "Negative max-delay slack $endpoint: $slack"
            if {$slack < $worst} {set worst $slack}; incr checked
        }
        incr total $checked
        lappend json_payloads "\{\"instance\":[m2c::json_string $inst],\"source_stem\":[m2c::json_string $inst/$src_stem],\"destination_stem\":[m2c::json_string $inst/$dst_stem],\"source_bits\":[m2c::int_array $sbits],\"expected_source_bits\":[m2c::int_array $sexpected],\"destination_bits\":[m2c::int_array $dbits],\"expected_destination_bits\":[m2c::int_array $dexpected],\"destination_count\":[llength $dst],\"timed_endpoints\":$checked,\"bus_skew_required\":$skew_required,\"requirement_ns\":$bound,\"worst_slack_ns\":$worst\}"
    }
    # Detect an added handshake bus not present in the reviewed channel table.
    set observed_parents {}
    foreach cell [get_cells -hier -quiet -include_replicated_objects * -filter "NAME =~ ${base}/*/req_ff_reg* && IS_SEQUENTIAL == 1"] {lappend observed_parents [file dirname [get_property NAME $cell]]}
    m2c::require {[lsort -unique $observed_parents] eq [lsort -unique $expected_parents]} "Undeclared/missing handshake parent: $observed_parents"
    set status_count 0
    foreach signal [dict get $::m2c_contract status_sources] {
        set d [get_pins -quiet [dict get $signal destination]]
        m2c::require {[llength $d] == 1} "Missing status synchronizer: $signal"
        set parent [file dirname [file dirname [get_property NAME $d]]]
        set stages [get_cells -hier -quiet -include_replicated_objects * -filter "NAME =~ ${parent}/sync_ff_reg* && IS_SEQUENTIAL == 1"]
        m2c::require {[m2c::bit_set $stages sync_ff_reg] eq {0 1 2}} "Wrong status synchronization depth: $parent"
        foreach reg $stages {m2c::require {[get_property ASYNC_REG $reg]} "Missing status ASYNC_REG: $reg"}
        set drivers [get_pins -quiet -leaf -of_objects [get_nets -segments -of_objects $d] -filter {DIRECTION == OUT}]
        m2c::require {[llength $drivers] == 1 && [get_property REF_PIN_NAME $drivers] eq "Q"} "Status source not direct registered Q: $d"
        set patterns [list [dict get $signal source]]
        if {[dict exists $signal source_aliases]} {lappend patterns {*}[dict get $signal source_aliases]}
        set matched 0; foreach pattern $patterns {if {[string match $pattern [get_property NAME $drivers]]} {set matched 1}}
        m2c::require {$matched} "Wrong registered source: $drivers"
        m2c::require {[get_property IS_SEQUENTIAL [get_cells -of_objects $drivers]]} {Status source is not sequential}
        incr status_count
    }
    m2c::require {$status_count >= 5} {Missing functional status checks}
    set shared_reset [m2c::check_shared_reset $base $clocks $::m2c_contract]
    set reset_checks [llength [dict get $::m2c_contract reset_xpm_owners]]
    set owner [get_cells -quiet $base/u_phy/phy_allow_sys_reg]
    m2c::require {[llength $owner] == 1 && [get_property IS_SEQUENTIAL $owner]} {Missing registered SYS reset owner}
    m2c::one_clock [get_pins -of_objects $owner -filter {REF_PIN_NAME == C}] 10.0 SYS_RESET_OWNER
    set ref_helper [get_cells -quiet $base/u_phy/u_ref_reset_sys_release/g_async.u_reset_sync]
    m2c::require {[llength $ref_helper] == 1} {Missing separate REF-to-SYS reset synchronizer}
    m2c::reset_sources [get_pins $base/u_phy/u_phy_sys_release/g_async.u_reset_sync/src_arst] $base/u_phy/phy_allow_sys_reg* $sys
    set raw_count_clr [get_pins -hier -quiet -regexp [format {^%s/.*/count_reg(\[[0-9]+\])?(_replica[^/]*)?/(CLR|PRE)$} $base]]
    m2c::require {[llength $raw_count_clr] == 0} {Counter still uses asynchronous CLR/PRE}
    set clr [get_pins -quiet $base/u_phy/u_rx_div/CLR]
    set clr_sources [all_fanin -flat -startpoints_only -to $clr]
    m2c::require {[llength $clr_sources] > 0} {Divider CLR lacks registered BITCLK qualification}
    foreach pin $clr_sources {
        set source_cell [get_cells -quiet -of_objects $pin]
        m2c::require {[llength $source_cell] == 1 && [get_property IS_SEQUENTIAL $source_cell]} "Divider CLR unregistered source: $pin"
        m2c::require {[get_clocks -quiet -of_objects [get_pins -of_objects $source_cell -filter {REF_PIN_NAME == C}]] eq $bclk} "Divider CLR released outside BITCLK domain: $pin"
    }
    file mkdir $reports
    set reset_timing [m2c::check_phy_reset $base $clocks [dict get $::m2c_contract reset_sequence] $reports]
    set output [file join $reports constraint_metrics.json]
    set f [open $output {WRONLY CREAT EXCL}]; fconfigure $f -encoding utf-8 -translation lf
    puts $f "\{\"schema\":1,\"verification_stage\":[m2c::json_string $stage],\"object_checks\":\"PASS\",\"phy_same_net_pairs\":$phy_pairs,\"registered_status_checks\":$status_count,\"reset_structure_checks\":$reset_checks,\"shared_reset_ownership\":$shared_reset,\"divider_clr_check\":\"PASS\",\"phy_reset_timing\":$reset_timing,\"timed_payload_endpoints\":$total,\"event_only_checks\":\[[join $json_events ,]\],\"payloads\":\[[join $json_payloads ,]\]\}"
    close $f
    puts "CLOSURE_NATIVE_OBJECT_GATE=PASS endpoints=$total channels=[llength $expected_parents]"
}
