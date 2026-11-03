# Native GUI session with non-blocking task dispatch. No project creation.
source [file join [file dirname [info script]] common.tcl]
m2real::open
set ::m2_gui_tasks [file join $m2real::delivery gui_tasks]
file mkdir $::m2_gui_tasks
proc m2_gui_poll {} {
    set task [file join $::m2_gui_tasks pending.tcl]
    if {[file exists $task]} {
        set active [file join $::m2_gui_tasks active.tcl]
        file rename -force $task $active
        set log [open [file join $::m2_gui_tasks result.txt] w]
        if {[catch {uplevel #0 [list source $active]} message options]} {
            puts $log "FAIL\n$message\n[dict get $options -errorinfo]"
        } else {puts $log "PASS\n$message"}
        close $log
    }
    after 500 m2_gui_poll
}
after 500 m2_gui_poll
puts {M2_GUI_READY}
