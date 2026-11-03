# Native visual export only. Opens current mainline read-only; never rearranges
# blocks or saves the BD. The project .ui is hashed independently before/after.
set root [file normalize [file join [file dirname [info script]] ../..]]
set out [file join $root delivery 20261013_c1 layout]
file mkdir $out
if {[catch {
    start_gui
    open_project -read_only [file join $root Radar_XCZU2CG_M2.xpr]
    open_bd_design [get_files */bd/radar_system/radar_system.bd]
    write_bd_layout -force -format native [file join $out radar_system.ui]
    write_bd_layout -force -format svg -orientation portrait [file join $out radar_system.svg]
    close_project
    puts {M2_CLOSURE_LAYOUT_EXPORT=PASS}
} message options]} {puts $message; puts [dict get $options -errorinfo]; exit 1}
exit 0
