# Standalone native unit project. Does not open/save the production XPR.
# vivado -mode batch -source run_native_unit.tcl -tclargs probe|cfar NEW_OUTPUT
if {[llength $argv] != 2} {error "Expected probe|cfar and a new output directory"}
set unit [lindex $argv 0]
set output [file normalize [lindex $argv 1]]
if {$unit ni {probe cfar}} {error "Unknown unit"}
if {[file exists $output]} {error "Output must not already exist"}
set source_root [file normalize [file join [file dirname [info script]] ../..]]
create_project native_unit $output -part xczu2cg-sfvc784-1-e
if {$unit eq "probe"} {
 foreach name {radar_fft_range radar_fft_doppler} {read_ip $source_root/ip/pl_dsp/$name/$name.xci}
 add_files -fileset sim_1 $source_root/sim/pl_dsp/tb_fft_scale.sv
 set_property top tb_fft_scale [get_filesets sim_1]
} else {
 add_files $source_root/rtl/dsp/radar_dsp_cfar.sv
 add_files -fileset sim_1 $source_root/sim/pl_dsp/tb_dsp_cfar.sv
 set_property top tb_dsp_cfar [get_filesets sim_1]
}
set_property xsim.simulate.runtime all [get_filesets sim_1]
launch_simulation
close_sim
close_project
exit
