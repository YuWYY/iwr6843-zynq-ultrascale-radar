source [file join [file dirname [info script]] common.tcl]
validate_bd_design
save_bd_design
generate_target all [get_files */bd/radar_system/radar_system.bd]
set wrappers [make_wrapper -files [get_files */bd/radar_system/radar_system.bd] -top]
foreach w $wrappers {if {![llength [get_files -quiet $w]]} {add_files -norecurse $w}}
set_property TOP radar_m2_top [get_filesets sources_1]
update_compile_order -fileset sources_1
m2real::require {[get_property SYNTH_CHECKPOINT_MODE [get_files */radar_system.bd]] eq "None"} {Global synthesis flow changed}
m2real::require {[llength [get_bd_ports]]==18} {Expected sixteen scalar signals plus the two physical members of the differential interface}
m2real::require {[llength [get_bd_intf_ports]]==1} {Expected one reference interface}
m2real::audit after
puts {M2_BD_VALIDATED_AND_GENERATED}
