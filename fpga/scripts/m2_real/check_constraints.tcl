# Read-only audit of an already open synthesized/implemented design.
# This file defines a procedure; it does not open/save a project or read XDC.
# Usage after opening synth_1 and reading radar_cdc.xdc:
#   source <project>/scripts/m2_real/check_constraints.tcl
#   m2_check_constraints <new-report-directory>
# The report directory is optional. PASS is an object/constraint check only.
namespace eval m2_constraint_audit {}

# Call only when an actual reviewed source-timing budget is supplied. The
# optional source-timing XDC remains declarative and is not loaded by this audit.
proc m2_validate_source_timing_values {} {
    foreach name {RADAR_IN_MIN_R_NS RADAR_IN_MAX_R_NS RADAR_IN_MIN_F_NS RADAR_IN_MAX_F_NS} {
        upvar #0 $name value
        if {![info exists value] || ![string is double -strict $value] || [regexp -nocase {nan|inf} $value]} {
            error "M2 SOURCE TIMING: missing/non-finite reviewed value $name"
        }
    }
    if {$::RADAR_IN_MIN_R_NS > $::RADAR_IN_MAX_R_NS || $::RADAR_IN_MIN_F_NS > $::RADAR_IN_MAX_F_NS} {
        error "M2 SOURCE TIMING: minimum exceeds maximum"
    }
    if {[llength [get_ports -quiet {radar_frclk_p radar_data0_p radar_data1_p}]] != 3 || [llength [get_clocks -quiet RADAR_BCLK300]] != 1} {
        error "M2 SOURCE TIMING: expected 3 positive input ports and one BITCLK clock"
    }
}

proc m2_constraint_audit::one_clock {pins expected_period label} {
    set clocks [get_clocks -quiet -of_objects $pins]
    if {[llength $clocks] != 1} {
        error "M2 AUDIT: $label requires exactly one clock, got [llength $clocks]"
    }
    set period [get_property PERIOD $clocks]
    if {abs($period - $expected_period) > 0.001} {
        error "M2 AUDIT: $label period=$period expected=$expected_period"
    }
    return $clocks
}

proc m2_check_constraints {{report_dir ""}} {
    if {![llength [get_cells -hier -quiet -include_replicated_objects * -filter {IS_PRIMITIVE == 1 && REF_NAME =~ FD*}]]} {
        error "M2 AUDIT: open a mapped netlist before running this audit"
    }
    # XDC files contain declarations only. All pin/electrical assertions belong
    # here, in full Tcl, so read_xdc cannot silently skip a Tcl control-flow body.
    foreach {name ball standard} {
        phy_ref_clk_100m_clk_p AE5 DIFF_SSTL12 phy_ref_clk_100m_clk_n AF5 DIFF_SSTL12
        radar_bitclk_p L7 LVDS radar_bitclk_n L6 LVDS
        radar_frclk_p L3 LVDS radar_frclk_n L2 LVDS
        radar_data0_p K8 LVDS radar_data0_n K7 LVDS
        radar_data1_p J6 LVDS radar_data1_n H6 LVDS
        radar_uart_tx AG10 LVCMOS33 radar_uart_rx AH10 LVCMOS33
        radar_nerr_n AE12 LVCMOS33 radar_3v3_present AF12 LVCMOS33
        radar_sync AE10 LVCMOS33 radar_nrst_n AF10 LVCMOS33
    } {
        set port [get_ports -quiet $name]
        if {[llength $port] != 1} { error "M2 AUDIT: expected one port $name" }
        if {[get_property PACKAGE_PIN $port] ne $ball || [get_property IOSTANDARD $port] ne $standard} {
            error "M2 AUDIT: pin/electrical assignment mismatch on $name"
        }
        if {$standard eq "LVDS" && ![get_property DIFF_TERM $port]} { error "M2 AUDIT: LVDS DIFF_TERM missing on $name" }
        if {$standard eq "DIFF_SSTL12" && [get_property DIFF_TERM $port]} { error "M2 AUDIT: REF100 internal termination must be off" }
    }
    foreach port [get_ports {radar_uart_tx radar_nrst_n radar_sync}] {
        if {[get_property DRIVE $port] != 4 || [get_property SLEW $port] ne "SLOW"} { error "M2 AUDIT: output drive/slew mismatch: $port" }
    }
    if {![get_property PULLDOWN [get_ports radar_3v3_present]]} { error "M2 AUDIT: 3V3-present pull-down missing" }
    puts "M2 AUDIT PINS: all 16 PACKAGE_PIN/IOSTANDARD assignments and termination/drive/slew/pulldown match; physical board still PENDING"
    set bclk_port [get_ports -quiet radar_bitclk_p]
    set ref_port [get_ports -quiet phy_ref_clk_100m_clk_p]
    if {[llength $bclk_port] != 1 || [llength $ref_port] != 1} {
        error "M2 AUDIT: primary clock ports missing"
    }
    set bclk [m2_constraint_audit::one_clock $bclk_port 3.333333 BITCLK]
    set refclk [m2_constraint_audit::one_clock $ref_port 10.000000 PL_REF100]
    if {[get_property NAME $bclk] ne "RADAR_BCLK300"} { error "M2 AUDIT: unexpected BITCLK name" }
    foreach {clock half} [list $bclk 1.666667 $refclk 5.000000] {
        set waveform [get_property WAVEFORM $clock]
        if {[llength $waveform] != 2 || abs([lindex $waveform 0]) > 0.001 || abs([lindex $waveform 1] - $half) > 0.001} {
            error "M2 AUDIT: unexpected primary waveform on $clock"
        }
    }
    set div_o [get_pins -hier -quiet -filter {NAME =~ */radar_capture_bd_0/*/u_phy/u_rx_div/O || NAME =~ */radar_capture_bd_0/u_phy/u_rx_div/O}]
    if {[llength $div_o] != 1} { error "M2 AUDIT: expected one RX divider O pin" }
    # Vivado quantizes the requested 3.333333ns primary period to 3.333ns.
    # Check the exact /4 relation to the realized master, not an unrounded value.
    set rx_period [expr {4.0 * [get_property PERIOD $bclk]}]
    set rxclk [m2_constraint_audit::one_clock $div_o $rx_period RX75]
    if {[get_property NAME $rxclk] ne "RADAR_RX75"} { error "M2 AUDIT: expected the single explicit RADAR_RX75 clock" }
    if {![get_property IS_GENERATED $rxclk] || [get_property MASTER_CLOCK $rxclk] ne "RADAR_BCLK300"} {
        error "M2 AUDIT: RX75 must be generated from RADAR_BCLK300"
    }
    set div_cell [get_cells -quiet -of_objects $div_o]
    if {[get_property REF_NAME $div_cell] ne "BUFGCE_DIV" || [get_property BUFGCE_DIVIDE $div_cell] != 4} {
        error "M2 AUDIT: RX divider primitive or divide value is wrong"
    }
    puts "M2 AUDIT CLOCKS: BITCLK=$bclk REF100=$refclk RX75=$rxclk master=[get_property MASTER_CLOCK $rxclk]"

    set parents [dict create]
    foreach cell [get_cells -hier -quiet -include_replicated_objects * -filter {NAME =~ */radar_capture_bd_0/*}] {
        set name [get_property NAME $cell]
        if {[regexp {(^|/)radar_capture_bd_0/(inst/)?(u_cmd_mb|u_phy/u_delay_mb|u_snapshot|u_tap_snapshot)$} $name -> prefix inner leaf]} {
            if {[dict exists $parents $leaf]} { error "M2 AUDIT: duplicate parent $leaf" }
            dict set parents $leaf $cell
        }
    }
    if {[dict size $parents] != 4} { error "M2 AUDIT: need 2 mailbox + 2 snapshot parents; got [dict keys $parents]" }
    if {$report_dir ne ""} { file mkdir $report_dir }
    set total_endpoints 0
    set records {}
    foreach {leaf src_stem dst_stem src_period dst_period} [list \
        u_cmd_mb src_hold_reg dst_data_reg 10.000000 $rx_period \
        u_phy/u_delay_mb src_hold_reg dst_data_reg 10.000000 3.333333 \
        u_snapshot rx_hold_reg data_reg $rx_period 10.000000 \
        u_tap_snapshot rx_hold_reg data_reg 3.333333 10.000000 \
    ] {
        if {![dict exists $parents $leaf]} { error "M2 AUDIT: missing $leaf" }
        set inst [dict get $parents $leaf]
        set src [get_cells -hier -quiet -include_replicated_objects * -filter "NAME =~ ${inst}/${src_stem}* && IS_SEQUENTIAL == 1"]
        set dst [get_cells -hier -quiet -include_replicated_objects * -filter "NAME =~ ${inst}/${dst_stem}* && IS_SEQUENTIAL == 1"]
        if {![llength $src] || ![llength $dst]} { error "M2 AUDIT: empty payload bus at $inst" }
        set src_clk [m2_constraint_audit::one_clock [get_pins -quiet -of_objects $src -filter {REF_PIN_NAME == C}] $src_period "$leaf source"]
        set dst_clk [m2_constraint_audit::one_clock [get_pins -quiet -of_objects $dst -filter {REF_PIN_NAME == C}] $dst_period "$leaf destination"]
        # Confirm the three-stage structures assumed in the 8ns derivation.
        foreach stem {req_ff ack_ff} {
            set seen [dict create]
            foreach ff [get_cells -hier -quiet -include_replicated_objects * -filter "NAME =~ ${inst}/${stem}_reg* && IS_SEQUENTIAL == 1"] {
                if {[regexp {/(req_ff|ack_ff)_reg\[([0-2])\](_replica[^/]*)?$} [get_property NAME $ff] -> type stage replica]} {
                    if {![get_property ASYNC_REG $ff]} { error "M2 AUDIT: ASYNC_REG missing on $ff" }
                    dict set seen $stage 1
                }
            }
            if {[dict size $seen] != 3} { error "M2 AUDIT: $inst/$stem is not a three-stage synchronizer" }
        }
        # Every surviving destination D, including physical replicas, must
        # retain a timed payload path. A
        # broad false path would make it disappear. Requirement must remain 8ns.
        set checked 0
        set worst_slack ""
        foreach endpoint [get_pins -quiet -of_objects $dst -filter {REF_PIN_NAME == D}] {
            set paths [get_timing_paths -from $src -to $endpoint -delay_type max -max_paths 1 -nworst 1]
            if {[llength $paths] != 1} { error "M2 AUDIT: no timed payload path to $endpoint" }
            set requirement [get_property REQUIREMENT $paths]
            if {abs($requirement - 8.000) > 0.001} {
                error "M2 AUDIT: $endpoint effective requirement=$requirement, expected 8ns"
            }
            set slack [get_property SLACK $paths]
            if {$worst_slack eq "" || $slack < $worst_slack} { set worst_slack $slack }
            incr checked
        }
        if {$checked != [llength $dst]} { error "M2 AUDIT: destination D count mismatch at $inst" }
        incr total_endpoints $checked
        set record [dict create instance $inst src_count [llength $src] dst_count [llength $dst] timed_endpoints $checked src_clock $src_clk dst_clock $dst_clk requirement_ns 8.000 worst_slack_ns $worst_slack]
        lappend records $record
        puts "M2 AUDIT PAYLOAD: $record"
        if {$report_dir ne ""} {
            set stem [string map {/ _} $leaf]
            report_exceptions -from $src -to $dst -coverage -file [file join $report_dir "${stem}_exception_coverage.rpt"]
            report_timing -from $src -to $dst -delay_type max -max_paths 8 -file [file join $report_dir "${stem}_payload_timing.rpt"]
        }
    }
    set first_d [get_pins -hier -quiet -regexp {^u_system/radar_system_i/radar_capture_bd_0/inst/.*/(sync_ff|req_ff|ack_ff)_reg\[0\](_replica[^/]*)?/D$}]
    set handshake_d [get_pins -hier -quiet -regexp {^u_system/radar_system_i/radar_capture_bd_0/inst/.*/(req_ff|ack_ff)_reg\[0\](_replica[^/]*)?/D$}]
    set release_async [get_pins -hier -quiet -regexp {^u_system/radar_system_i/radar_capture_bd_0/inst/(.*/)?u_(src_release|dst_release|sys_release|rx_release|ref_release|deser_release|fifo_release|rx_control_release|cfg_release)/(permit_ff_reg\[[0-9]+\]|count_reg\[[0-9]+\]|released_reg)(_replica[^/]*)?/(CLR|PRE)$}]
    if {![llength $first_d] || [llength $handshake_d] < 8} { error "M2 AUDIT: first-stage exception targets missing; req/ack=[llength $handshake_d]" }
    if {![llength $release_async]} { error "M2 AUDIT: reset-assertion exception targets missing" }
    if {[llength [get_ports -quiet {radar_uart_tx radar_nrst_n radar_sync}]] != 3} { error "M2 AUDIT: 3 low-speed output targets required" }
    puts "M2 AUDIT EXCEPTION_TARGETS: first_D=[llength $first_d] req_ack_first_D=[llength $handshake_d] reset_assertion=[llength $release_async] low_speed_outputs=3"
    set released_pins {}
    foreach ff [get_cells -hier -quiet -include_replicated_objects * -filter {NAME =~ */radar_capture_bd_0/* && IS_SEQUENTIAL == 1}] {
        if {[regexp {/u_(src_release|dst_release|sys_release|rx_release|ref_release|deser_release|fifo_release|rx_control_release|cfg_release)/released_reg(_replica[^/]*)?$} [get_property NAME $ff]]} {
            foreach pin [get_pins -quiet -of_objects $ff -filter {REF_PIN_NAME == CLR || REF_PIN_NAME == PRE}] { lappend released_pins $pin }
        }
    }
    if {![llength $released_pins]} { error "M2 AUDIT: released_reg asynchronous assertion pins missing" }
    puts "M2 AUDIT RESET_ASSERTION: [llength $released_pins] released_reg pins; review exact exceptions in reports"
    if {$report_dir ne ""} {
        report_clocks -file [file join $report_dir clocks.rpt]
        check_timing -verbose -file [file join $report_dir check_timing.rpt]
        report_exceptions -summary -file [file join $report_dir exceptions_summary.rpt]
        report_exceptions -coverage -file [file join $report_dir exceptions_coverage.rpt]
        report_exceptions -ignored -file [file join $report_dir exceptions_ignored.rpt]
        report_exceptions -ignored_objects -file [file join $report_dir exceptions_ignored_objects.rpt]
        report_exceptions -to $first_d -coverage -file [file join $report_dir first_stage_exception_coverage.rpt]
        report_exceptions -to $release_async -coverage -file [file join $report_dir reset_assertion_exception_coverage.rpt]
        report_bus_skew -max_paths 16 -file [file join $report_dir bus_skew.rpt]
        report_cdc -details -file [file join $report_dir cdc.rpt]
        set output [open [file join $report_dir constraint_audit.txt] w]
        puts $output "OBJECT_AND_REQUIREMENT_CHECKS=PASS"
        puts $output "MAILBOX_PARENTS=2 SNAPSHOT_PARENTS=2 TIMED_PAYLOAD_ENDPOINTS=$total_endpoints"
        foreach record $records { puts $output $record }
        puts $output "SOURCE_TIMING=PENDING; PHYSICAL_BOARD=PENDING; TIMING_SIGNOFF_NOT_CLAIMED"
        close $output
    }
    puts "M2 AUDIT: OBJECT_AND_REQUIREMENT_CHECKS=PASS; payload_endpoints=$total_endpoints"
    puts "M2 AUDIT: Review slack, bus skew, exception coverage and CDC reports; SOURCE_TIMING=PENDING; PHYSICAL_BOARD=PENDING."
    return $records
}
