namespace eval m2real {
    variable root [file normalize [file join [file dirname [info script]] ../..]]
    variable build_id 20261012_171114
    variable out [file join $root outputs m2_real_$build_id]
    variable delivery [file join $root delivery $build_id]
    proc require {condition message} {if {![uplevel 1 [list expr $condition]]} {error $message}}
    proc open {} {
        variable root
        if {![llength [current_project -quiet]]} {open_project [file join $root Radar_XCZU2CG_M2.xpr]}
        require {[file normalize [get_property DIRECTORY [current_project]]] eq $root} {Wrong open project directory}
        require {[get_property PART [current_project]] eq "xczu2cg-sfvc784-1-e"} {Wrong part}
        require {[version -short] eq "2025.1"} {Wrong Vivado version}
        open_bd_design [get_files */bd/radar_system/radar_system.bd]
    }
    proc config {} {
        set result [dict create]
        foreach name [lsort [get_bd_cells]] {
            if {$name eq "/radar_capture_bd_0" || $name eq "radar_capture_bd_0"} {continue}
            foreach prop [lsort [list_property [get_bd_cells $name]]] {
                if {[string match CONFIG.* $prop]} {dict set result "$name/$prop" [get_property $prop [get_bd_cells $name]]}
            }
        }
        return $result
    }
    proc width {name} {
        set l [get_property LEFT [get_bd_pins $name]]
        set r [get_property RIGHT [get_bd_pins $name]]
        if {$l eq "" || $r eq ""} {return 1}
        return [expr {abs($l-$r)+1}]
    }
    proc audit {label} {
        variable delivery
        set dir [file join $delivery $label]
        file mkdir $dir
        set f [::open [file join $dir config.tcl] w]
        puts $f [list set ::m2real_config [config]]
        close $f
        set f [::open [file join $dir networks.tsv] w]
        puts $f "kind\tname\tendpoints"
        foreach n [lsort [get_bd_nets]] {puts $f "signal\t$n\t[join [lsort [concat [get_bd_pins -quiet -of_objects [get_bd_nets $n]] [get_bd_ports -quiet -of_objects [get_bd_nets $n]]]]]"}
        foreach n [lsort [get_bd_intf_nets]] {puts $f "interface\t$n\t[join [lsort [concat [get_bd_intf_pins -quiet -of_objects [get_bd_intf_nets $n]] [get_bd_intf_ports -quiet -of_objects [get_bd_intf_nets $n]]]]]"}
        close $f
        set f [::open [file join $dir addresses.tsv] w]
        puts $f "space\tsegment\toffset\trange"
        foreach sp [lsort [get_bd_addr_spaces]] {
            foreach sg [lsort [get_bd_addr_segs -quiet -of_objects [get_bd_addr_spaces $sp]]] {
                puts $f "$sp\t$sg\t[get_property OFFSET [get_bd_addr_segs $sg]]\t[get_property RANGE [get_bd_addr_segs $sg]]"
            }
        }
        close $f
        set f [::open [file join $dir pins.tsv] w]
        puts $f "pin\tdir\twidth\ttype\tfrequency\tdomain"
        foreach p [lsort [get_bd_pins -hier]] {
            set vals [list $p [get_property DIR [get_bd_pins $p]] [width $p] [get_property TYPE [get_bd_pins $p]]]
            foreach prop {CONFIG.FREQ_HZ CONFIG.CLK_DOMAIN} {
                if {[lsearch -exact [list_property [get_bd_pins $p]] $prop]>=0} {lappend vals [get_property $prop [get_bd_pins $p]]} else {lappend vals {}}
            }
            puts $f [join $vals \t]
        }
        close $f
        write_bd_tcl -force [file join $dir radar_system.tcl]
        puts "M2_AUDIT $label CELLS=[llength [get_bd_cells]] SIGNAL_NETS=[llength [get_bd_nets]] INTERFACE_NETS=[llength [get_bd_intf_nets]]"
    }
}
