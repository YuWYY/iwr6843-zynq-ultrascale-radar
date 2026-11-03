source [file join [file dirname [info script]] common.tcl]
if {[catch {
    m2real::open
    source [file join $m2real::root scripts m2_real integrate_current.tcl]
    source [file join $m2real::root scripts m2_real verify_integrated.tcl]
    close_project
} msg opts]} {puts $msg; puts [dict get $opts -errorinfo]; exit 1}
exit 0
