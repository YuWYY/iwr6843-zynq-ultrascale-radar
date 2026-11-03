# Read-only active-reference manifest after the final native save.
set here [file dirname [info script]]
source [file join $here common.tcl]
m2real::open
set f [open [file join $m2real::delivery active_references.tsv] w]
puts $f "path\texists\ttype\tused_in"
foreach item [lsort [get_files -all]] {
    set path [file normalize $item]
    puts $f "$path\t[file exists $path]\t[get_property FILE_TYPE [get_files -all $item]]\t[get_property USED_IN [get_files -all $item]]"
}
close $f
report_compile_order -fileset sources_1 -file [file join $m2real::delivery compile_order.rpt]
report_compile_order -fileset sim_1 -file [file join $m2real::delivery simulation_compile_order.rpt]
close_project
exit 0
