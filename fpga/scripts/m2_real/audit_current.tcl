# Read-only design inspection. The caller opens the project; this script does
# not validate, save, retune, connect, or create design objects.
source [file join [file dirname [info script]] common.tcl]
if {![llength [current_bd_design -quiet]]} {error {Open radar_system before auditing}}
m2real::audit audit_[clock format [clock seconds] -format %Y%m%d_%H%M%S]
