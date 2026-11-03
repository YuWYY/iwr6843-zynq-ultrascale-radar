# Native offline synthesis/route. No connection to hardware is permitted here.
# Usage: vivado -mode batch -source build.tcl -tclargs synth|route|export <evidence-dir>
if {[llength $argv] != 2} {error {Expected stage evidence_dir}}
lassign $argv stage out
if {$stage ni {synth route export}} {error {Unknown stage}}
set out [file normalize $out]
file mkdir $out
set root [file normalize [file join [file dirname [info script]] ../..]]
set status [open $out/${stage}_status.txt w];puts $status RUNNING;close $status
if {[catch {
    open_project $root/Radar_XCZU2CG_M2.xpr
    if {[get_property PART [current_project]] ne "xczu2cg-sfvc784-1-e" || [version -short] ne "2025.1"} {error {Wrong tool or part}}
    if {[get_property TOP [get_filesets sources_1]] ne "radar_m2_top"} {error {Wrong design top}}
    set f [open $out/fft_plan_contract.txt w]
    foreach {name length} {radar_fft_range 256 radar_fft_doppler 32} {
        set ip [get_ips -quiet $name]
        if {[llength $ip] != 1} {error "Missing unique FFT IP: $name"}
        foreach {key expected} [list implementation_options radix_2_burst_io \
            transform_length $length input_width 24 phase_factor_width 24 \
            scaling_options scaled rounding_modes convergent_rounding \
            output_ordering natural_order throttle_scheme nonrealtime] {
            set actual [get_property CONFIG.$key $ip]
            puts $f "$name $key=$actual"
            if {$actual ne $expected} {error "Approved FFT contract mismatch: $name $key=$actual expected=$expected"}
        }
    }
    close $f
    set bd [get_files */radar_system.bd]
    if {[get_property SYNTH_CHECKPOINT_MODE $bd] ne "None"} {error {Global BD synthesis required}}
    foreach run {synth_1 impl_1} {
        set_property AUTO_INCREMENTAL_CHECKPOINT 0 [get_runs $run]
        set_property INCREMENTAL_CHECKPOINT {} [get_runs $run]
    }
    set_param general.maxThreads 4
    if {$stage eq "synth"} {
        open_bd_design $bd
        validate_bd_design
        generate_target all $bd
        update_compile_order -fileset sources_1
        reset_run synth_1
        launch_runs synth_1 -jobs 4
        wait_on_run synth_1
        if {[get_property STATUS [get_runs synth_1]] ne "synth_design Complete!"} {error "Synthesis: [get_property STATUS [get_runs synth_1]]"}
        open_run synth_1
    } elseif {$stage eq "route"} {
        if {[get_property STATUS [get_runs synth_1]] ne "synth_design Complete!" || [get_property NEEDS_REFRESH [get_runs synth_1]]} {error {Synthesis absent or stale}}
        reset_run impl_1
        launch_runs impl_1 -to_step route_design -jobs 4
        wait_on_run impl_1
        # Preserve detailed reports even when native routing finishes with a
        # timing failure. The explicit timing/DRC gates below still reject it.
        if {![string match "route_design Complete*" [get_property STATUS [get_runs impl_1]]]} {error "Implementation: [get_property STATUS [get_runs impl_1]]"}
        open_run impl_1
    } else {
        set impl_status [get_property STATUS [get_runs impl_1]]
        if {$impl_status ni {"route_design Complete!" "write_bitstream Complete!"} || [get_property NEEDS_REFRESH [get_runs impl_1]]} {error {Passing route absent or stale}}
        # write_hw_platform -include_bit obtains the bit from the managed run.
        # A standalone write_bitstream to another directory does not complete
        # that run step and is not sufficient for a matched include-bit XSA.
        if {$impl_status ne "write_bitstream Complete!"} {
            launch_runs impl_1 -to_step write_bitstream -jobs 4
            wait_on_run impl_1
            if {[get_property STATUS [get_runs impl_1]] ne "write_bitstream Complete!"} {error "Bitstream: [get_property STATUS [get_runs impl_1]]"}
        }
        open_run impl_1
    }
    report_utilization -hierarchical -file $out/${stage}_utilization.rpt
    report_timing_summary -report_unconstrained -file $out/${stage}_timing.rpt
    report_drc -file $out/${stage}_drc.rpt
    report_cdc -details -file $out/${stage}_cdc.rpt
    report_clocks -file $out/${stage}_clocks.rpt
    report_clock_interaction -file $out/${stage}_clock_interaction.rpt
    report_exceptions -coverage -file $out/${stage}_exceptions.rpt
    check_timing -verbose -file $out/${stage}_check_timing.rpt
    if {$stage in {synth route}} {
        source $root/scripts/m2_closure/check_constraints.tcl
        m2c_check_constraints $out $root/scripts/m2_closure/cdc_contract.tcl $stage
    }
    if {$stage ne "synth"} {
        report_route_status -file $out/${stage}_route_status.rpt
        report_io -file $out/${stage}_io.rpt
        set paths [get_timing_paths -quiet -delay_type max -max_paths 1]
        if {![llength $paths] || [get_property SLACK $paths] < 0} {error {Setup timing not met}}
        set paths [get_timing_paths -quiet -delay_type min -max_paths 1]
        if {![llength $paths] || [get_property SLACK $paths] < 0} {error {Hold timing not met}}
        set bad [get_drc_violations -quiet -filter {SEVERITY == Error || SEVERITY == {Critical Warning}}]
        if {[llength $bad]} {error "Unresolved DRC: $bad"}
    }
    if {$stage eq "export"} {
        # Offline artifact only. Native bitgen still applies its complete DRC.
        set native_bit [file join [get_property DIRECTORY [get_runs impl_1]] radar_m2_top.bit]
        if {![file isfile $native_bit]} {error {Managed run bitstream is missing}}
        file copy -force $native_bit $out/radar_system_m2_offline.bit
        write_hw_platform -fixed -include_bit -force $out/radar_system_m2_offline.xsa
    }
    set f [open $out/${stage}_native_state.txt w]
    puts $f "PART=[get_property PART [current_project]]"
    puts $f "SYNTH=[get_property STATUS [get_runs synth_1]]"
    puts $f "IMPL=[get_property STATUS [get_runs impl_1]]"
    puts $f {BOARD=UNVERIFIED}
    close $f
    close_project
} message options]} {
    set status [open $out/${stage}_status.txt w];puts $status "FAILED\n$message";close $status
    puts [dict get $options -errorinfo]
    exit 1
}
set status [open $out/${stage}_status.txt w];puts $status {PASS_OFFLINE_BOARD_UNVERIFIED};close $status
exit 0
