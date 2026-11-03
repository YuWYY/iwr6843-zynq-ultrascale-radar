# Current dependency inventory only: no Validate, regeneration or save.
# Optional -tclargs <absolute-report-directory>; only report files are written.
set root [file normalize [file join [file dirname [info script]] ../..]]
set out [file join $root delivery 20261013_c1]
if {[catch {
    if {[llength $argv] > 1} {error {Expected at most one absolute report directory}}
    if {[llength $argv] == 1} {
        if {[file pathtype [lindex $argv 0]] ne "absolute"} {error {Report directory must be absolute}}
        set out [file normalize [lindex $argv 0]]
    }
    file mkdir $out
    open_project -read_only [file join $root Radar_XCZU2CG_M2.xpr]
    set f [open [file join $out active_references.tsv] w]
    puts $f "path\texists\ttype\tused_in"
    foreach item [lsort [get_files -all]] {
        set obj [get_files -all $item]
        set path [file normalize $item]
        puts $f "$path\t[file exists $path]\t[get_property FILE_TYPE $obj]\t[get_property USED_IN $obj]"
    }
    close $f
    report_compile_order -fileset sources_1 -file [file join $out compile_order.rpt]
    report_compile_order -fileset sim_1 -file [file join $out simulation_compile_order.rpt]
    close_project
    puts {M2_CLOSURE_REFERENCES=PASS}
} message options]} {puts $message; puts [dict get $options -errorinfo]; exit 1}
exit 0
