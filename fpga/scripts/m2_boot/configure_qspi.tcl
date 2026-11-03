# Native incremental enablement of the fitted CM02-2CG(C) single QSPI.
# No hardware connection or Flash access. Preserve all radar/DP/DDR interfaces.
if {[llength $argv] != 1} {error {Expected evidence directory}}
set out [file normalize [lindex $argv 0]]
file mkdir $out
set root [file normalize [file join [file dirname [info script]] ../..]]
proc dump_ps {ps path} {
    set result [dict create]
    set f [open $path w]
    foreach p [lsort [list_property $ps CONFIG.*]] {
        set v [get_property $p $ps]
        dict set result $p $v
        puts $f [list $p $v]
    }
    close $f
    return $result
}
proc topology {} {
    set result [dict create]
    foreach net [get_bd_nets -quiet] {
        dict set result net:$net [lsort [concat [get_bd_pins -quiet -of_objects $net] [get_bd_ports -quiet -of_objects $net]]]
    }
    foreach net [get_bd_intf_nets -quiet] {
        dict set result intf:$net [lsort [concat [get_bd_intf_pins -quiet -of_objects $net] [get_bd_intf_ports -quiet -of_objects $net]]]
    }
    foreach cell [get_bd_cells -quiet] {dict set result cell:$cell [get_property VLNV $cell]}
    foreach seg [get_bd_addr_segs -quiet -hier] {
        dict set result addr:$seg [list [get_property OFFSET $seg] [get_property RANGE $seg]]
    }
    return $result
}
set f [open $out/status.txt w];puts $f RUNNING;close $f
if {[catch {
    open_project $root/Radar_XCZU2CG_M2.xpr
    if {[version -short] ne "2025.1" || [get_property PART [current_project]] ne "xczu2cg-sfvc784-1-e"} {error {Wrong tool/device}}
    set bd [get_files */radar_system.bd]
    if {[llength $bd] != 1} {error {Expected unique radar_system BD}}
    open_bd_design $bd
    set ps [get_bd_cells -filter {VLNV =~ *:zynq_ultra_ps_e:*}]
    if {[llength $ps] != 1} {error {Expected unique PS}}
    write_bd_tcl -force $out/before_bd.tcl
    set before [dump_ps $ps $out/before_ps.tcl]
    set before_topo [topology]
    set f [open $out/before_topology.tcl w];dict for {k v} $before_topo {puts $f [list $k $v]};close $f
    if {[get_property CONFIG.PSU_BANK_0_IO_STANDARD $ps] ne "LVCMOS18"} {error {QSPI bank must already be 1.8V}}
    if {[get_property CONFIG.PSU__SD0__PERIPHERAL__ENABLE $ps] ne "0" || [get_property CONFIG.PSU__SD1__PERIPHERAL__ENABLE $ps] ne "0"} {error {Unexpected enabled SD interface}}
    set expected [list CONFIG.PSU__QSPI__PERIPHERAL__ENABLE 1 \
        CONFIG.PSU__QSPI__PERIPHERAL__IO {MIO 0 .. 5} \
        CONFIG.PSU__QSPI__PERIPHERAL__MODE Single \
        CONFIG.PSU__QSPI__PERIPHERAL__DATA_MODE x4 \
        CONFIG.PSU__QSPI__GRP_FBCLK__ENABLE 0 \
        CONFIG.PSU__CRL_APB__QSPI_REF_CTRL__FREQMHZ 200]
    set_property -dict $expected $ps
    foreach {p v} $expected {if {[get_property $p $ps] ne $v} {error "QSPI readback failed: $p"}}
    validate_bd_design
    set after [dump_ps $ps $out/after_ps.tcl]
    set f [open $out/ps_changes.tcl w]
    set unexpected {}
    dict for {p old} $before {
        set new [dict get $after $p]
        if {$old eq $new} {continue}
        puts $f [list $p $old $new]
        if {$p eq "CONFIG.PSU__PROTECTION__MASTERS"} {
            if {[string map {QSPI:NonSecure;0 QSPI:NonSecure;1} $old] ne $new} {lappend unexpected $p}
            continue
        }
        if {$p eq "CONFIG.PSU__PROTECTION__SLAVES"} {
            set old_entries [split $old |];set new_entries [split $new |]
            if {[llength $old_entries] != [llength $new_entries]} {lappend unexpected $p;continue}
            foreach a $old_entries b $new_entries {
                if {$a eq $b} {continue}
                if {![string match *QSPI* $a] || [string range $a 0 end-1] ne [string range $b 0 end-1] || [string index $a end] ne "0" || [string index $b end] ne "1"} {lappend unexpected $p}
            }
            continue
        }
        if {![string match *QSPI* $p] && ![regexp {^CONFIG\.PSU_MIO_[0-5]_DIRECTION$} $p] && $p ni {CONFIG.PSU_MIO_TREE_PERIPHERALS CONFIG.PSU_MIO_TREE_SIGNALS CONFIG.PSU_MIO_TREE_PERIPHERALS_VALUE}} {
            lappend unexpected $p
        }
    }
    close $f
    if {[llength $unexpected]} {error "Unreviewed derived property changes (not saved): $unexpected"}
    set after_topo [topology]
    set f [open $out/after_topology.tcl w];dict for {k v} $after_topo {puts $f [list $k $v]};close $f
    set changed_topo {}
    foreach k [lsort -unique [concat [dict keys $before_topo] [dict keys $after_topo]]] {
        # Native PS exposes its fixed QSPI slave aperture when enabled. This
        # adds no master mapping; every existing DDR/CSR mapping stays exact.
        if {$k eq "addr:/zynq_ultra_ps_e_0/SAXIGP2/HP0_QSPI" && ![dict exists $before_topo $k] && [dict exists $after_topo $k] && [dict get $after_topo $k] eq "{} 0x20000000"} {continue}
        if {![dict exists $before_topo $k] || ![dict exists $after_topo $k] || [dict get $before_topo $k] ne [dict get $after_topo $k]} {lappend changed_topo $k}
    }
    if {[llength $changed_topo]} {error "BD topology/address changes requiring review: $changed_topo"}
    set f [open $out/protected_topology.tcl w];puts $f $before_topo;close $f
    save_bd_design
    write_bd_tcl -force $out/after_bd.tcl
    generate_target all $bd
    update_compile_order -fileset sources_1
    close_project
} message options]} {
    set f [open $out/status.txt w];puts $f "FAILED\n$message";close $f
    puts [dict get $options -errorinfo]
    exit 1
}
set f [open $out/status.txt w];puts $f PASS_NATIVE_QSPI_CONFIG;close $f
exit 0
