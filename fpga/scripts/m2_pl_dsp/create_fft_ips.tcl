# Native IP creation only. Safe to source in either the main project or a
# separate IP-generation project. Do not edit generated XCI content.
proc radar_create_fft_ips {} {
    set root [file normalize [file join [file dirname [info script]] ../..]]
    # info script is empty when a proc runs after source; use the stored root.
    set root $::radar_fft_source_root
    set out [file join $root ip pl_dsp]
    file mkdir $out
    foreach {name length} {radar_fft_range 256 radar_fft_doppler 32} {
        set existing [get_ips -quiet $name]
        if {[llength $existing] == 0} {
            set xci [file join $out $name ${name}.xci]
            if {[file exists $xci]} { read_ip $xci } else {
                create_ip -name xfft -vendor xilinx.com -library ip -version 9.1 -module_name $name -dir $out
            }
        }
        set_property -dict [list CONFIG.transform_length $length \
            CONFIG.implementation_options {radix_2_burst_io} \
            CONFIG.data_format {fixed_point} CONFIG.input_width {24} \
            CONFIG.phase_factor_width {24} CONFIG.scaling_options {scaled} \
            CONFIG.rounding_modes {convergent_rounding} \
            CONFIG.output_ordering {natural_order} \
            CONFIG.throttle_scheme {nonrealtime} \
            CONFIG.aresetn {true} CONFIG.ovflo {true}] [get_ips $name]
        generate_target all [get_ips $name]
        export_ip_user_files -of_objects [get_ips $name] -no_script -sync -force -quiet
        puts "RADAR_FFT_READY $name [get_property IP_FILE [get_ips $name]]"
    }
}
set ::radar_fft_source_root [file normalize [file join [file dirname [info script]] ../..]]
