# Writable native refresh of current sources/module reference; no BD rebuilding.
source [file join [file dirname [info script]] .. m2_real common.tcl]
set m2real::delivery [file join $m2real::root delivery 20261013_c1]
proc closure_files {directory pattern} {
    set result [glob -nocomplain -directory $directory -types f $pattern]
    foreach sub [glob -nocomplain -directory $directory -types d *] {
        set result [concat $result [closure_files $sub $pattern]]
    }
    return [lsort $result]
}
if {[catch {
    m2real::open
    set before_config [m2real::config]
    m2real::require {[llength [get_bd_cells]] == 12} {Unexpected BD cell count}
    m2real::require {[llength [get_bd_cells -filter {VLNV =~ "*:module_ref:radar_capture_bd:*"}]] == 1} {Expected one capture module}
    foreach path [concat [closure_files [file join $m2real::root rtl] *.v] [closure_files [file join $m2real::root rtl] *.sv]] {
        if {![llength [get_files -quiet $path]]} {add_files -fileset sources_1 -norecurse $path}
        m2real::require {[llength [get_files -all -quiet $path]] == 1} "Duplicate source $path"
    }
    foreach path [glob -nocomplain -directory [file join $m2real::root sim] *.sv] {
        if {![llength [get_files -quiet $path]]} {add_files -fileset sim_1 -norecurse $path}
    }
    set_property XPM_LIBRARIES {XPM_CDC XPM_MEMORY XPM_FIFO} [current_project]
    set_property TOP radar_m2_top [get_filesets sources_1]
    set_property TOP tb_capture_fifo [get_filesets sim_1]
    set_property TOP_AUTO_SET false [get_filesets sim_1]
    update_compile_order -fileset sources_1
    set module_ip [get_property CONFIG.Component_Name [get_bd_cells radar_capture_bd_0]]
    update_module_reference [get_ips $module_ip]
    set cdc [get_files [file join $m2real::root constraints radar_cdc.xdc]]
    set_property USED_IN_SYNTHESIS false $cdc
    set_property PROCESSING_ORDER LATE $cdc
    validate_bd_design -force
    set after_config [m2real::config]
    m2real::require {$before_config eq $after_config} {Official IP configuration changed during native refresh}
    m2real::require {[llength [get_bd_nets]] == 37 && [llength [get_bd_intf_nets]] == 9} {BD topology changed}
    m2real::require {[llength [get_bd_ports]] == 18 && [llength [get_bd_intf_ports]] == 1} {External BD ports changed}
    save_bd_design
    generate_target all [get_files */bd/radar_system/radar_system.bd]
    foreach wrapper [make_wrapper -files [get_files */bd/radar_system/radar_system.bd] -top] {
        if {![llength [get_files -quiet $wrapper]]} {add_files -fileset sources_1 -norecurse $wrapper}
    }
    update_compile_order -fileset sources_1
    update_compile_order -fileset sim_1
    foreach run {synth_1 impl_1} {
        m2real::require {![get_property AUTO_INCREMENTAL_CHECKPOINT [get_runs $run]]} "Incremental enabled $run"
        m2real::require {[get_property INCREMENTAL_CHECKPOINT [get_runs $run]] eq ""} "Incremental DCP retained $run"
    }
    m2real::require {[get_property SYNTH_CHECKPOINT_MODE [get_files */radar_system.bd]] eq "None"} {Expected Global flow}
    m2real::audit after
    close_project
    puts {M2_CLOSURE_NATIVE_REFRESH=PASS}
} msg opts]} {puts $msg; puts [dict get $opts -errorinfo]; exit 1}
exit 0
