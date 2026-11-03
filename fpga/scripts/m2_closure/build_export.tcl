# Caller launches one fresh Vivado batch process. Never programs hardware.
# -tclargs <synth|route|dev-hw|candidate> <unique-build-id> <absolute-SIM_PASS.json>
# dev-hw/candidate reuse ONLY the same identity-qualified current routed build.
# Export stops at NATIVE_COMPLETE. After Vivado exits, run hardware_export.py
# seal with its closed native session log and the actual process exit code.
set root [file normalize [file join [file dirname [info script]] ../..]]
source [file join $root scripts m2_closure native_export.tcl]
set m2c::native_execution FAIL
set m2c::simulation_state NOT_RUN
set m2c::download_candidate NO
if {[llength $argv] != 3 || [lindex $argv 0] ni {synth route dev-hw candidate}} {error {Expected stage build_id simulation_gate}}
lassign $argv stage build_id simulation
if {![regexp {^[A-Za-z0-9_-]+$} $build_id]} {error {Unsafe build_id}}
set out [file join $root outputs m2_closure_$build_id]
file mkdir $out
set status_file [file join $out ${stage}_STATUS.json]
if {[file exists $status_file]} {error "Stage already attempted: $status_file; use another unique build_id"}
proc m2c::status {path stage id state {message {}}} {
    variable native_execution
    variable simulation_state
    variable download_candidate
    set f [open $path w]; fconfigure $f -encoding utf-8 -translation lf
    puts $f "\{\"stage\":[m2c::json_string $stage],\"build_id\":[m2c::json_string $id],\"state\":[m2c::json_string $state],\"SIMULATION\":[m2c::json_string $simulation_state],\"NATIVE_EXECUTION\":[m2c::json_string $native_execution],\"DOWNLOAD_CANDIDATE\":[m2c::json_string $download_candidate],\"message\":[m2c::json_string $message]\}"
    close $f
}
proc m2c::export_hardware {root out mode simulation native_before} {
    set export [file join $out $mode]
    if {[file exists $export]} {error "Export attempt already exists: $export"}
    file mkdir $export
    set helper [file join $root scripts m2_closure hardware_export.py]
    set arguments [list --mode $mode --root $root --build $out --native $native_before --simulation $simulation]
    # Re-evaluates current native inputs, actual DCP/run logs, and sealed reports.
    # Candidate PENDING fails here, before opening bitgen or creating any bit.
    puts [exec python -B -I $helper preflight {*}$arguments]
    if {[get_property STATUS [get_runs impl_1]] ne "route_design Complete!" || [get_property NEEDS_REFRESH [get_runs impl_1]]} {
        error {Current implementation is not an unchanged, completed routed run}
    }
    open_run impl_1
    set bitgen_completed false
    set include_bit false
    set bitgen_command {}
    if {$mode eq "candidate"} {
        set actual_bit [file join [get_property DIRECTORY [get_runs impl_1]] radar_m2_top.bit]
        if {[file exists $actual_bit]} {error "An implementation bit already exists; do not reuse or overwrite it: $actual_bit"}
        # Direct native bitgen retains the sealed route run's original runme.log.
        # write_bitstream executes its full native DRC; no -bin_file or waiver.
        write_bitstream $actual_bit
        if {![file isfile $actual_bit] || [file size $actual_bit] == 0} {error {Native bitgen produced no bit}}
        report_drc -ruledeck bitstream -file [file join $export bitgen_drc.rpt]
        file copy $actual_bit [file join $export radar_system_m2.bit]
        set bitgen_completed true
        set include_bit true
        set bitgen_command write_bitstream
        set xsa [file join $export radar_system_m2.xsa]
        write_hw_platform -fixed -include_bit $xsa
    } else {
        set xsa [file join $export radar_system_m2_dev_hw.xsa]
        write_hw_platform -fixed $xsa
    }
    if {![file isfile $xsa] || [file size $xsa] == 0} {error {Native hardware export produced no XSA}}
    set receipt [open [file join $export native_export_receipt.json] {WRONLY CREAT EXCL}]
    fconfigure $receipt -encoding utf-8 -translation lf
    puts $receipt "\{\"mode\":[m2c::json_string $mode],\"tool_version\":[m2c::json_string [version -short]],\"top\":[m2c::json_string [get_property TOP [get_filesets sources_1]]],\"run_status\":[m2c::json_string [get_property STATUS [get_runs impl_1]]],\"completed\":true,\"include_bit\":$include_bit,\"include_bin\":false,\"bitgen_command\":[m2c::json_string $bitgen_command],\"bitgen_completed\":$bitgen_completed\}"
    close $receipt
    set native_after [file join $out ${mode}_native_after.json]
    m2c::export_native $root $native_after
    # The process-wide Vivado -log is still open. The host seals its complete
    # bytes and verifies artifacts after this process has exited successfully.
}
m2c::status $status_file $stage $build_id RUNNING
if {[catch {
    if {[llength [current_project -quiet]]} {error {Fresh batch process required}}
    open_project [file join $root Radar_XCZU2CG_M2.xpr]
    if {[file normalize [get_property DIRECTORY [current_project]]] ne $root} {error {Wrong main project}}
    if {[get_property PART [current_project]] ne "xczu2cg-sfvc784-1-e" || [version -short] ne "2025.1"} {error {Wrong part/tool}}
    if {[get_property TOP [get_filesets sources_1]] ne "radar_m2_top" || [get_property TOP [get_filesets sim_1]] ne "tb_capture_fifo"} {error {Wrong top/sim top}}
    foreach run {synth_1 impl_1} {
        if {[get_property AUTO_INCREMENTAL_CHECKPOINT [get_runs $run]] || [get_property INCREMENTAL_CHECKPOINT [get_runs $run]] ne ""} {error "Incremental enabled: $run"}
    }
    set bd [get_files */bd/radar_system/radar_system.bd]
    if {[llength $bd] != 1 || [get_property SYNTH_CHECKPOINT_MODE $bd] ne "None"} {error {Global BD synthesis required}}
    set_param general.maxThreads 4
    update_compile_order -fileset sources_1
    set native_before [file join $out ${stage}_native_before.json]
    m2c::export_native $root $native_before
    set identity [file join $root scripts m2_closure build_identity.py]
    set manifest [file join $out build_manifest.json]
    set synth_receipt [file join $out synth_identity.json]
    set synth_dcp [file join [get_property DIRECTORY [get_runs synth_1]] radar_m2_top.dcp]
    set args_identity [list --root $root --native $native_before --simulation $simulation --manifest $manifest]
    if {$stage in {dev-hw candidate}} {
        m2c::export_hardware $root $out $stage $simulation $native_before
        set m2c::simulation_state PASS
        set m2c::native_execution PASS
        m2c::status $status_file $stage $build_id NATIVE_COMPLETE {Native export completed; host log/artifact qualification is still required}
        close_project
        puts "M2_EXPORT_NATIVE_COMPLETE $stage $build_id"
        exit 0
    }
    if {$stage eq "synth"} {
        puts [exec python -B -I $identity record {*}$args_identity --build-id $build_id]
        set m2c::simulation_state PASS
        reset_run synth_1
        launch_runs synth_1 -jobs 4
        wait_on_run synth_1
        if {[get_property STATUS [get_runs synth_1]] ne "synth_design Complete!"} {error "Synthesis failed: [get_property STATUS [get_runs synth_1]]"}
        set m2c::native_execution PASS
        open_run synth_1
        read_xdc -no_add [file join $root constraints radar_cdc.xdc]
    } else {
        puts [exec python -B -I $identity require-synth {*}$args_identity --dcp $synth_dcp --receipt $synth_receipt]
        set m2c::simulation_state PASS
        if {[get_property STATUS [get_runs synth_1]] ne "synth_design Complete!"} {error {Actual synth run not complete}}
        reset_run impl_1
        launch_runs impl_1 -to_step route_design -jobs 4
        wait_on_run impl_1
        if {[get_property STATUS [get_runs impl_1]] ne "route_design Complete!"} {error "Route failed: [get_property STATUS [get_runs impl_1]]"}
        set m2c::native_execution PASS
        open_run impl_1
    }
    set reports [file join $out reports_$stage]
    file mkdir $reports
    # Run subprocesses have exited; seal complete logs, not this still-open launcher log.
    puts [exec python -B -I $identity capture-logs {*}$args_identity --reports $reports --stage $stage]
    report_utilization -file [file join $reports utilization.rpt]
    report_timing_summary -report_unconstrained -file [file join $reports timing.rpt]
    report_drc -file [file join $reports drc.rpt]
    report_cdc -details -file [file join $reports cdc.rpt]
    report_clocks -file [file join $reports clocks.rpt]
    report_clock_interaction -file [file join $reports clock_interaction.rpt]
    report_exceptions -coverage -file [file join $reports exceptions_coverage.rpt]
    report_exceptions -ignored -file [file join $reports exceptions_ignored.rpt]
    check_timing -verbose -file [file join $reports check_timing.rpt]
    report_bus_skew -max_paths 16 -file [file join $reports bus_skew.rpt]
    if {$stage eq "route"} {
        report_io -file [file join $reports io.rpt]
        report_route_status -file [file join $reports route_status.rpt]
    }
    source [file join $root scripts m2_closure check_constraints.tcl]
    m2c_check_constraints $reports [file join $root scripts m2_closure cdc_contract.tcl] $stage
    puts [exec python -B -I [file join $root scripts m2_closure qualify_payload.py] $reports --stage $stage]
    if {$stage eq "route"} {
        source [file join $root scripts m2_closure capture_r07_evidence.tcl]
        m2r07_capture $root $reports $stage
    }
    set native_after [file join $out ${stage}_native_after.json]
    m2c::export_native $root $native_after
    set args_after [list --root $root --native $native_after --simulation $simulation --manifest $manifest]
    if {$stage eq "synth"} {
        puts [exec python -B -I $identity seal-synth {*}$args_after --dcp $synth_dcp --receipt $synth_receipt]
        m2c::status $status_file $stage $build_id PASS
    } else {
        set actual_routed [file join [get_property DIRECTORY [get_runs impl_1]] radar_m2_top_routed.dcp]
        set route_receipt [file join $out route_identity.json]
        puts [exec python -B -I $identity seal-route {*}$args_after --dcp $synth_dcp --receipt $synth_receipt --routed-dcp $actual_routed --reports $reports --output $route_receipt]
        m2c::status $status_file $stage $build_id NATIVE_COMPLETE
        # Nonzero qualification does not erase the fact that native routing ran.
        set gate_args [list --root $root --reports $reports --route-receipt $route_receipt --stage-status $status_file --policy [file join $root scripts m2_closure release_policy.json] --output [file join $out release_status.json] --native $native_after --manifest $manifest --simulation $simulation --synth-receipt $synth_receipt --synth-dcp $synth_dcp]
        if {[catch {exec python -B -I [file join $root scripts m2_closure release_gate.py] {*}$gate_args} qualified options]} {
            puts $qualified
            close_project
            exit 2
        }
        puts $qualified
    }
    close_project
} msg options]} {
    m2c::status $status_file $stage $build_id FAIL $msg
    puts $msg; puts [dict get $options -errorinfo]
    if {[llength [current_project -quiet]]} {close_project}
    exit 1
}
exit 0
