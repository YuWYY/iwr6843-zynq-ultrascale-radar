# Run: vivado -mode batch -source build_export.tcl -tclargs <synth|route> <build_id>
# Source-timing/electrical signoff remains PENDING: this entry never writes a bit.
source [file join [file dirname [info script]] common.tcl]
if {[llength $argv]!=2 || [lindex $argv 0] ni {synth route}} {error {Expected stage (synth or route) and unique build_id}}
set stage [lindex $argv 0]
set m2real::build_id [lindex $argv 1]
set m2real::out [file join $m2real::root outputs m2_real_$m2real::build_id]
file mkdir [file join $m2real::out reports]
set flag [file join $m2real::out ${stage}_STATUS.txt]
if {[file exists $flag]} {error "Build stage already attempted: $flag; use a new build_id"}
set f [open $flag w];puts $f RUNNING;close $f
if {[catch {
    m2real::open
    set_param general.maxThreads 4
    # Ignore Vivado's embedded PYTHONHOME/PYTHONPATH when invoking host Python.
    puts [exec python -I [file join $m2real::root scripts m2_real simulation_gate.py] --check]
    set input_manifest [file join $m2real::out ${stage}_input_hashes.json]
    puts [exec python -I [file join $m2real::root scripts m2_real build_manifest.py] record $input_manifest]
    foreach r {synth_1 impl_1} {
        m2real::require {![get_property AUTO_INCREMENTAL_CHECKPOINT [get_runs $r]]} "Auto incremental enabled on $r"
        m2real::require {[get_property INCREMENTAL_CHECKPOINT [get_runs $r]] eq ""} "Incremental DCP set on $r"
    }
    m2real::require {[get_property TOP [get_filesets sources_1]] eq "radar_m2_top"} {Wrong physical top}
    if {$stage eq "synth"} {
        reset_run synth_1
        launch_runs synth_1 -jobs 4
        wait_on_run synth_1
        m2real::require {[get_property STATUS [get_runs synth_1]] eq "synth_design Complete!"} "Synthesis failed: [get_property STATUS [get_runs synth_1]]"
        open_run synth_1
        # CDC paths use synthesized register names and remain active in implementation.
        read_xdc -no_add [file join $m2real::root constraints radar_cdc.xdc]
        source [file join $m2real::root scripts m2_real check_constraints.tcl]
        report_utilization -file [file join $m2real::out reports utilization_synth.rpt]
        report_drc -file [file join $m2real::out reports drc_synth.rpt]
        report_cdc -details -file [file join $m2real::out reports cdc_synth.rpt]
        report_clocks -file [file join $m2real::out reports clocks_synth.rpt]
        report_clock_interaction -file [file join $m2real::out reports clock_interaction_synth.rpt]
        report_timing_summary -report_unconstrained -file [file join $m2real::out reports timing_synth.rpt]
        write_checkpoint -force [file join $m2real::out radar_m2_top_synth.dcp]
        m2_check_constraints [file join $m2real::out reports constraints_synth]
    } else {
        m2real::require {[get_property STATUS [get_runs synth_1]] eq "synth_design Complete!"} {Synthesis not complete}
        reset_run impl_1
        launch_runs impl_1 -to_step route_design -jobs 4
        wait_on_run impl_1
        m2real::require {[get_property STATUS [get_runs impl_1]] eq "route_design Complete!"} "Implementation failed: [get_property STATUS [get_runs impl_1]]"
        open_run impl_1
        source [file join $m2real::root scripts m2_real check_constraints.tcl]
        report_utilization -file [file join $m2real::out reports utilization_route.rpt]
        report_timing_summary -report_unconstrained -file [file join $m2real::out reports timing_route.rpt]
        report_drc -file [file join $m2real::out reports drc_route.rpt]
        report_cdc -details -file [file join $m2real::out reports cdc_route.rpt]
        report_clocks -file [file join $m2real::out reports clocks_route.rpt]
        report_clock_interaction -file [file join $m2real::out reports clock_interaction_route.rpt]
        report_io -file [file join $m2real::out reports io_route.rpt]
        report_route_status -file [file join $m2real::out reports route_status.rpt]
        report_exceptions -coverage -file [file join $m2real::out reports exceptions_coverage.rpt]
        check_timing -verbose -file [file join $m2real::out reports check_timing.rpt]
        write_checkpoint -force [file join $m2real::out radar_m2_top_routed_provisional.dcp]
        m2_check_constraints [file join $m2real::out reports constraints_route]
    }
    puts [exec python -I [file join $m2real::root scripts m2_real build_manifest.py] check $input_manifest]
    close_project
    set f [open $flag w];puts $f "PASS $stage\nSOURCE_TIMING=PENDING\nBOARD_ELECTRICAL=PENDING\nNOT_DOWNLOAD_READY";close $f
} msg opts]} {
    set f [open $flag w];puts $f "FAIL $stage\n$msg";close $f
    puts $msg;puts [dict get $opts -errorinfo];exit 1
}
exit 0
