# Read-only native audit. No Validate, save, or target generation.
source [file join [file dirname [info script]] .. m2_real common.tcl]
set m2real::delivery [file join $m2real::root delivery 20261013_c1]
set label audit_[clock format [clock seconds] -format %Y%m%d_%H%M%S]
if {[llength $argv]} {set label [lindex $argv 0]}
if {[llength $argv] > 1 || ![regexp {^[A-Za-z0-9_-]+$} $label]} {error {Expected optional new audit label}}
if {[file exists [file join $m2real::delivery $label]]} {error {Audit label already exists; preserve earlier evidence}}
if {[catch {
    open_project -read_only [file join $m2real::root Radar_XCZU2CG_M2.xpr]
    m2real::open
    m2real::audit $label
    set f [open [file join $m2real::delivery $label active_references.tsv] w]
    puts $f "path\texists\ttype\tused_in"
    foreach item [lsort [get_files -all]] {
        puts $f "$item\t[file exists $item]\t[get_property FILE_TYPE [get_files -all $item]]\t[get_property USED_IN [get_files -all $item]]"
    }
    close $f
    close_project
} msg opts]} {puts $msg; puts [dict get $opts -errorinfo]; exit 1}
exit 0
