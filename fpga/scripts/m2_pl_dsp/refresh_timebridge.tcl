# Native refresh of only the existing DSP module reference; no IP reconfiguration.
if {[llength $argv]!=1} {error {Expected evidence directory}}
set out [file normalize [lindex $argv 0]]
file mkdir $out
set root [file normalize [file join [file dirname [info script]] ../..]]
proc protected_state {} {
    set state {}
    foreach cell [lsort [get_bd_cells]] {
        foreach p [lsort [list_property $cell]] {
            if {[string match CONFIG.* $p]} {lappend state [list $cell $p [get_property $p $cell]]}
        }
    }
    foreach net [lsort [get_bd_nets]] {
        lappend state [list NET $net [lsort [get_bd_pins -of_objects $net]] [lsort [get_bd_ports -of_objects $net]]]
    }
    foreach net [lsort [get_bd_intf_nets]] {
        lappend state [list INTF $net [lsort [get_bd_intf_pins -of_objects $net]] [lsort [get_bd_intf_ports -of_objects $net]]]
    }
    foreach seg [lsort [get_bd_addr_segs -hier]] {
        lappend state [list ADDR $seg [get_property OFFSET $seg] [get_property RANGE $seg]]
    }
    return $state
}
if {[catch {
    open_project $root/Radar_XCZU2CG_M2.xpr
    if {[version -short] ne "2025.1" || [get_property PART [current_project]] ne "xczu2cg-sfvc784-1-e"} {error {Unexpected tool or device}}
    set bd [get_files */radar_system.bd]
    if {[llength $bd]!=1 || [get_property SYNTH_CHECKPOINT_MODE $bd] ne "None"} {error {Expected one Global BD}}
    update_compile_order -fileset sources_1
    open_bd_design $bd
    write_bd_tcl -force $out/before_bd.tcl
    set before [protected_state]
    set module_ip [get_property CONFIG.Component_Name [get_bd_cells radar_dsp_accel_0]]
    update_module_reference [get_ips $module_ip]
    validate_bd_design
    set after [protected_state]
    set f [open $out/protected_before.txt w];puts $f [join $before \n];close $f
    set f [open $out/protected_after.txt w];puts $f [join $after \n];close $f
    if {$before ne $after} {error {Protected configuration, network or address changed}}
    save_bd_design
    write_bd_tcl -force $out/after_bd.tcl
    close_project
} message options]} {
    set f [open $out/status.txt w];puts $f "FAILED\n$message";close $f
    puts [dict get $options -errorinfo]
    exit 1
}
set f [open $out/status.txt w];puts $f PASS;close $f
exit 0
