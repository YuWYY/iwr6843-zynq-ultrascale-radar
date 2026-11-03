# Export semantic build inputs from the actual open Vivado project. No saves.
namespace eval m2c {}
proc m2c::json_string {value} {
    return "\"[string map [list \\ \\\\ \" \\\" \n \\n \r \\r \t \\t] $value]\""
}
proc m2c::json_array {values} {
    set out {}; foreach value $values {lappend out [m2c::json_string $value]}
    return "\[[join $out ,]\]"
}
proc m2c::json_dict {values} {
    set out {}; foreach key [lsort [dict keys $values]] {lappend out "[m2c::json_string $key]:[m2c::json_string [dict get $values $key]]"}
    return "\{[join $out ,]\}"
}
proc m2c::props {object names} {
    set found [list_property $object]; set result [dict create]
    foreach name $names {if {$name in $found} {dict set result $name [get_property $name $object]}}
    return $result
}
proc m2c::export_native {root destination} {
    if {![llength [current_project -quiet]]} {error {Open current project before native input export}}
    if {[file normalize [get_property DIRECTORY [current_project]]] ne [file normalize $root]} {error {Wrong project root}}
    if {[file exists $destination]} {error "Native inventory already exists: $destination"}
    set project [m2c::props [current_project] {PART TARGET_LANGUAGE SIMULATOR_LANGUAGE DEFAULT_LIB XPM_LIBRARIES IP_REPO_PATHS TARGET_SIMULATOR}]
    set sets {}; foreach name {sources_1 constrs_1 sim_1} {
        set obj [get_filesets $name]
        set p [m2c::props $obj {TOP TOP_AUTO_SET INCLUDE_DIRS VERILOG_DEFINE GENERIC LIBRARY XPM_LIBRARIES}]
        lappend sets "[m2c::json_string $name]:[m2c::json_dict $p]"
    }
    set runs {}; foreach name {synth_1 impl_1} {
        set obj [get_runs $name]; set names {DIRECTORY PART FLOW STRATEGY SRCSET CONSTRSET AUTO_INCREMENTAL_CHECKPOINT INCREMENTAL_CHECKPOINT}
        foreach p [list_property $obj] {if {[string match STEPS.* $p]} {lappend names $p}}
        lappend runs "[m2c::json_string $name]:[m2c::json_dict [m2c::props $obj [lsort -unique $names]]]"
    }
    set order {}; set order_files {}; set implementation_files {}
    # RTL projects do not have an implementation *source* compile order: the
    # mapped source is the actual synth-run DCP, sealed separately by SHA256.
    # Preserve both constraint orders plus all enabled implementation inputs.
    foreach {label kind stage} {synthesis_sources sources synthesis synthesis_constraints constraints synthesis implementation_constraints constraints implementation} {
        set names {}; foreach obj [get_files -compile_order $kind -used_in $stage] {
            set name [file normalize [get_property NAME $obj]]; lappend names $name; lappend order_files $name
        }
        if {![llength $names]} {error "Empty native $label compile order"}
        lappend order "[m2c::json_string $label]:[m2c::json_array $names]"
    }
    set records {}; set seen [dict create]
    foreach obj [get_files -all] {
        set p [m2c::props $obj {FILE_TYPE LIBRARY USED_IN USED_IN_SYNTHESIS USED_IN_IMPLEMENTATION USED_IN_SIMULATION IS_ENABLED IS_GLOBAL_INCLUDE SCOPED_TO_REF SCOPED_TO_CELLS PROCESSING_ORDER SYNTH_CHECKPOINT_MODE}]
        set name [file normalize [get_property NAME $obj]]
        set active [expr {$name in $order_files}]
        foreach flag {USED_IN_SYNTHESIS USED_IN_IMPLEMENTATION} {if {[dict exists $p $flag] && [dict get $p $flag]} {set active 1}}
        if {[dict exists $p IS_ENABLED] && ![dict get $p IS_ENABLED]} {set active 0}
        if {!$active} {continue}
        if {[dict exists $p USED_IN_IMPLEMENTATION] && [dict get $p USED_IN_IMPLEMENTATION]} {lappend implementation_files $name}
        if {[dict exists $seen $name]} {continue}; dict set seen $name 1
        if {![file isfile $name]} {error "Active native input missing: $name"}
        lappend records "\{\"path\":[m2c::json_string $name],\"properties\":[m2c::json_dict $p]\}"
    }
    if {![llength $records]} {error {Empty active file inventory}}
    if {![llength $implementation_files]} {error {Empty enabled implementation input inventory}}
    lappend order "\"implementation_inputs\":[m2c::json_array [lsort -unique $implementation_files]]"
    file mkdir [file dirname $destination]
    set f [open $destination {WRONLY CREAT EXCL}]
    fconfigure $f -encoding utf-8 -translation lf
    puts $f "\{\"schema\":1,\"project_root\":[m2c::json_string [file normalize $root]],\"tool_version\":[m2c::json_string [version -short]],\"project\":[m2c::json_dict $project],\"filesets\":\{[join $sets ,]\},\"runs\":\{[join $runs ,]\},\"compile_order\":\{[join $order ,]\},\"files\":\[[join [lsort $records] ,]\]\}"
    close $f
}
