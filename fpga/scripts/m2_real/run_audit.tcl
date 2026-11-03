source [file join [file dirname [info script]] common.tcl]
if {[catch {
    m2real::open
    source [file join $m2real::root scripts m2_real audit_current.tcl]
    help update_module_reference
    help make_wrapper
    help remove_files
    help delete_ip_run
    report_property [get_runs synth_1]
    report_property [get_files */radar_system.bd]
    close_project
} msg opts]} {puts $msg; puts [dict get $opts -errorinfo]; exit 1}
exit 0
