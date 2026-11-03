# Native artifact verification only; run in a fresh Vivado batch process.
# -tclargs <absolute-extracted-project-root> <absolute-outside-report-directory>
#          [<expected-config.tcl>]
# Default evidence: <extraction-root>/evidence/delivery/after/config.tcl.
# Launch with -log <output-directory>/reopen_native.log and
# -journal <output-directory>/reopen_native.jou to retain all native messages.
# No synthesis, generation, saves, manual XCI/BD edits, or deletion occurs here.
# Only the disposable extracted XPR is opened. Native validation requires a
# normally opened project and may update its in-memory model; no save is issued.

namespace eval m2_reopen {
    variable original /fpga
    variable tool C:/Xilinx/2025.1
    variable expected_cells {
        axi_dma_capture axi_gpio_ctrl axis_fifo_capture clk_wiz_phy_ref
        const_one const_zero radar_capture_bd_0 rst_phy_ref_300m rst_sys_100m
        smartconnect_ctrl smartconnect_data zynq_ultra_ps_e_0
    }

    proc require {condition message} {
        if {![uplevel 1 [list expr $condition]]} { error $message }
    }

    proc inside {path base} {
        set p [string tolower [string map {\\ /} [file normalize $path]]]
        set b [string trimright [string tolower [string map {\\ /} [file normalize $base]]] /]
        return [expr {$p eq $b || [string first "$b/" $p] == 0}]
    }

    proc read_text {path} {
        require {[file isfile $path]} "Required file missing: $path"
        set channel [open $path r]
        fconfigure $channel -encoding utf-8
        set text [read $channel]
        close $channel
        return $text
    }

    proc config_dictionary {path} {
        # Parse the exported 'set ::m2real_config <dict>' as a Tcl list. Do not
        # source evidence as executable code or import a mainline common.tcl.
        set text [string trim [read_text $path]]
        require {[llength $text] == 3 && [lindex $text 0] eq "set" && [lindex $text 1] eq "::m2real_config"} "Unexpected expected-config format: $path"
        set expected [lindex $text 2]
        require {[dict size $expected] > 0} "Empty expected IP parameter map"
        set protected {}
        dict for {key value} $expected {
            require {[regexp {^/([^/]+)/(CONFIG\..+)$} $key -> cell prop]} "Malformed config key: $key"
            require {$cell ne "radar_capture_bd_0"} "Capture RTL module must not be in the protected-IP map"
            dict set protected $cell 1
        }
        require {[dict size $protected] == 11} "Expected exactly 11 protected IPs, found [dict keys $protected]"
        return $expected
    }

    proc check_config {expected report} {
        set channel [open $report w]
        puts $channel "cell\tproperty\texpected\tactual\tresult"
        set failures {}
        dict for {key value} $expected {
            regexp {^/([^/]+)/(CONFIG\..+)$} $key -> name prop
            set cell [get_bd_cells -quiet $name]
            require {[llength $cell] == 1} "Protected IP missing/duplicated: $name"
            set actual [get_property $prop $cell]
            if {$actual ne $value} {
                lappend failures $key
                set outcome DIFF
            } else { set outcome MATCH }
            puts $channel [join [list $name $prop $value $actual $outcome] \t]
        }
        close $channel
        require {![llength $failures]} "Protected IP parameter mismatch: $failures"
        return [dict size $expected]
    }

    proc check_references {copy report} {
        variable original
        variable tool
        set channel [open $report w]
        puts $channel "path\texists\ttype\tused_in\tlocation"
        set checked 0
        set inactive_missing 0
        set external_tools 0
        foreach item [get_files -all] {
            set path [file normalize [get_property NAME $item]]
            set normalized [string map {\\ /} $path]
            require {![inside $path $original]} "Original-mainline dependency in extracted project: $path"
            require {![regexp -nocase {(^|/)Downloads(/|$)} $normalized]} "Downloads dependency in extracted project: $path"
            if {[inside $path $copy]} {
                set location extracted_project
            } elseif {[inside $path $tool]} {
                set location external_tool
                incr external_tools
            } else { error "Dependency outside extracted project/approved tool root: $path" }
            set used [get_property USED_IN $item]
            set exists [file isfile $path]
            if {!$exists && [llength $used]} { error "Required active input missing in extracted project: $path ($used)" }
            if {!$exists} { incr inactive_missing }
            puts $channel [join [list $path $exists [get_property FILE_TYPE $item] $used $location] \t]
            incr checked
        }
        close $channel
        require {$checked > 0} "No project file references were returned"
        puts "REOPEN REFERENCES: files=$checked inactive_missing=$inactive_missing external_tools=$external_tools original_mainline=0 downloads=0"
        return $checked
    }

    proc networks {} {
        set result [dict create]
        foreach net [get_bd_nets] {
            set endpoints [lsort [concat [get_bd_pins -quiet -of_objects $net] [get_bd_ports -quiet -of_objects $net]]]
            dict set result "signal\t$net" $endpoints
        }
        foreach net [get_bd_intf_nets] {
            set endpoints [lsort [concat [get_bd_intf_pins -quiet -of_objects $net] [get_bd_intf_ports -quiet -of_objects $net]]]
            dict set result "interface\t$net" $endpoints
        }
        return $result
    }

    proc expected_networks {path} {
        set result [dict create]
        set rows [split [string trim [read_text $path]] \n]
        require {[string trimright [lindex $rows 0] \r] eq "kind\tname\tendpoints"} "Unexpected networks.tsv header"
        foreach row [lrange $rows 1 end] {
            set fields [split [string trimright $row \r] \t]
            require {[llength $fields] == 3} "Malformed network row: $row"
            lassign $fields kind name endpoints
            dict set result "$kind\t$name" [lsort $endpoints]
        }
        require {[dict size $result] == 46} "Expected 37 signal + 9 interface network records"
        return $result
    }

    proc check_structure {expected_nets} {
        variable expected_cells
        set actual_cells {}
        foreach cell [get_bd_cells] { lappend actual_cells [string trimleft [get_property NAME $cell] /] }
        require {[lsort $actual_cells] eq [lsort $expected_cells]} "Unexpected BD cell set: $actual_cells"
        require {[llength [get_bd_cells -quiet -filter {VLNV =~ *:module_ref:radar_capture_bd:*}]] == 1} "Expected one radar_capture_bd module reference"
        require {[llength [get_bd_cells -quiet -filter {VLNV =~ *:axi_gpio:*}]] == 1} "Expected one AXI GPIO"
        require {[llength [get_bd_nets]] == 37} "Expected 37 BD signal nets"
        require {[llength [get_bd_intf_nets]] == 9} "Expected 9 BD interface nets"
        # Native get_bd_ports includes the P/N members of the differential
        # interface. The JSON BD stores 16 scalar ports plus that one interface.
        require {[llength [get_bd_ports]] == 18} "Expected 18 native BD port members"
        require {[llength [get_bd_intf_ports]] == 1} "Expected one reference-clock BD interface"
        set actual [networks]
        require {[lsort [dict keys $actual]] eq [lsort [dict keys $expected_nets]]} "Network names differ from packaged expected evidence"
        dict for {key endpoints} $expected_nets {
            require {[dict get $actual $key] eq $endpoints} "Network endpoints changed: $key"
        }
    }

    proc check_hdl_ports {copy project_name} {
        set top [read_text [file join $copy rtl top radar_m2_top.v]]
        regsub -all {//[^\n]*} $top {} top
        require {[regexp -indices {\mmodule\s+radar_m2_top\s*\(} $top opening]} "Physical top module/header not found"
        set begin [expr {[lindex $opening 1] + 1}]
        require {[regexp -indices -start $begin {\)\s*;} $top closing]} "Physical top port-list terminator missing"
        set header [string range $top $begin [expr {[lindex $closing 0] - 1}]]
        set names {}
        foreach declaration [split $header ,] {
            require {[regexp {^\s*(input|output|inout)\s+(wire\s+)?([A-Za-z_][A-Za-z0-9_]*)\s*$} $declaration -> direction wire name]} "Non-scalar/unexpected physical port declaration: $declaration"
            lappend names $name
        }
        set expected {
            phy_ref_clk_100m_clk_p phy_ref_clk_100m_clk_n radar_bitclk_p radar_bitclk_n
            radar_frclk_p radar_frclk_n radar_data0_p radar_data0_n radar_data1_p radar_data1_n
            radar_uart_rx radar_nerr_n radar_3v3_present radar_uart_tx radar_nrst_n radar_sync
        }
        require {[lsort $names] eq [lsort $expected]} "Physical top must contain exactly the 16 reviewed scalar ports"
        set wrapper_path [file join $copy ${project_name}.gen sources_1 bd radar_system hdl radar_system_wrapper.v]
        set wrapper [read_text $wrapper_path]
        set matches [regexp -all -inline -line {^\s*(input|output|inout)\s+([A-Za-z_][A-Za-z0-9_]*)\s*;} $wrapper]
        set wrapper_names {}
        foreach {whole direction name} $matches { lappend wrapper_names $name }
        set expected_wrapper {
            phy_ref_clk_100m_clk_n phy_ref_clk_100m_clk_p radar_3v3_raw
            radar_bitclk_n radar_bitclk_p radar_data0_n radar_data0_p radar_data1_n radar_data1_p
            radar_frclk_n radar_frclk_p radar_nerr_n_raw radar_nrst_low radar_sync_data
            radar_sync_oe radar_uart_rx_raw radar_uart_tx_data radar_uart_tx_oe
        }
        require {[lsort $wrapper_names] eq [lsort $expected_wrapper]} "Generated BD wrapper must contain the 18 reviewed scalar members"
        set active_wrapper [get_files -all -quiet $wrapper_path]
        require {[llength $active_wrapper] == 1} "Generated native wrapper is not an active project input"
        puts "REOPEN HDL: native_wrapper_members=18 physical_top_scalar_ports=16"
    }
}

if {[llength $argv] < 2 || [llength $argv] > 3} {
    error "Expected extracted_project_root output_report_directory ?expected_config.tcl?"
}
set copy_root [file normalize [lindex $argv 0]]
set output_dir [file normalize [lindex $argv 1]]
m2_reopen::require {[file pathtype [lindex $argv 0]] eq "absolute" && [file pathtype [lindex $argv 1]] eq "absolute"} "Use explicit absolute copy and output paths"
m2_reopen::require {[file isdirectory $copy_root]} "Extracted project directory missing"
m2_reopen::require {![m2_reopen::inside $copy_root $m2_reopen::original]} "Refusing to open the original mainline or any child directory"
m2_reopen::require {![m2_reopen::inside $output_dir $copy_root]} "Reports must be outside the extracted project"
file mkdir $output_dir
set status_path [file join $output_dir reopen_status.txt]
m2_reopen::require {![file exists $status_path]} "Reopen report already exists; choose a fresh report directory"
set status [open $status_path w]
puts $status "RUNNING\nCOPY=$copy_root"
close $status
set reopened_project 0

if {[catch {
    m2_reopen::require {![llength [current_project -quiet]]} "Run this script in a fresh process; an existing project must not be reused"
    m2_reopen::require {[version -short] eq "2025.1"} "Expected Vivado 2025.1"
    set xprs [glob -nocomplain -directory $copy_root *.xpr]
    m2_reopen::require {[llength $xprs] == 1} "Expected exactly one extracted XPR"
    set xpr [lindex $xprs 0]
    set project_name [file rootname [file tail $xpr]]
    set expected_config [file join [file dirname $copy_root] evidence delivery after config.tcl]
    if {[llength $argv] == 3} { set expected_config [file normalize [lindex $argv 2]] }
    set expected [m2_reopen::config_dictionary $expected_config]
    set expected_nets [m2_reopen::expected_networks [file join [file dirname $expected_config] networks.tsv]]
    # Vivado 2025.1 refuses validate_bd_design for a read-only project
    # (Project 1-228). This verified disposable copy is opened normally;
    # the mainline is explicitly refused above and no save is requested.
    open_project $xpr
    set reopened_project 1
    m2_reopen::require {[string equal -nocase [file normalize [get_property DIRECTORY [current_project]]] $copy_root]} "Opened project DIRECTORY differs from extracted copy"
    m2_reopen::require {[get_property PART [current_project]] eq "xczu2cg-sfvc784-1-e"} "Unexpected target part"
    m2_reopen::require {[get_property TOP [get_filesets sources_1]] eq "radar_m2_top"} "Unexpected physical top"
    m2_reopen::require {[get_property TOP [get_filesets sim_1]] eq "tb_capture_fifo"} "Unexpected simulation top"
    foreach name {synth_1 impl_1} {
        set run [get_runs -quiet $name]
        m2_reopen::require {[llength $run] == 1} "Expected run missing: $name"
        m2_reopen::require {![get_property AUTO_INCREMENTAL_CHECKPOINT $run]} "Automatic incremental synthesis/implementation enabled: $name"
        m2_reopen::require {[get_property INCREMENTAL_CHECKPOINT $run] eq ""} "Incremental checkpoint retained: $name"
    }
    m2_reopen::require {![llength [get_files -quiet -of_objects [get_filesets utils_1] -filter {FILE_TYPE == "Design Checkpoint"}]]} "Stale DCP remains in utils_1"
    set bd_path [file join $copy_root ${project_name}.srcs sources_1 bd radar_system radar_system.bd]
    m2_reopen::require {[file isfile $bd_path]} "Native radar_system.bd missing"
    set bd [get_files -all -quiet $bd_path]
    m2_reopen::require {[llength $bd] == 1} "Native BD is not uniquely active"
    m2_reopen::require {[get_property SYNTH_CHECKPOINT_MODE $bd] eq "None"} "BD is not in global synthesis mode None"
    set file_count [m2_reopen::check_references $copy_root [file join $output_dir active_references_before.tsv]]
    m2_reopen::check_hdl_ports $copy_root $project_name
    open_bd_design $bd
    m2_reopen::check_structure $expected_nets
    set config_count [m2_reopen::check_config $expected [file join $output_dir protected_config_before.tsv]]
    # Authorised native validation is scoped to the disposable extracted copy.
    # Keep the complete native messages in the launcher's Vivado -log file.
    # Do not waive errors, suppress messages, or save the BD.
    puts "M2_REOPEN_NATIVE_VALIDATE_BEGIN: $copy_root"
    validate_bd_design -force
    puts "M2_REOPEN_NATIVE_VALIDATE_END: PASS"
    m2_reopen::check_structure $expected_nets
    m2_reopen::check_config $expected [file join $output_dir protected_config_after.tsv]
    m2_reopen::check_references $copy_root [file join $output_dir active_references_after.tsv]
    report_compile_order -fileset sources_1 -file [file join $output_dir compile_order.rpt]
    report_compile_order -fileset sim_1 -file [file join $output_dir simulation_compile_order.rpt]
    close_project
    set status [open $status_path w]
    puts $status "PASS\nCOPY=$copy_root\nPART=xczu2cg-sfvc784-1-e\nTOP=radar_m2_top\nSIM_TOP=tb_capture_fifo\nBD_SYNTH_CHECKPOINT_MODE=None\nINCREMENTAL=DISABLED\nBD_CELLS=12\nCAPTURE_MODULES=1\nAXI_GPIO=1\nSIGNAL_NETS=37\nINTERFACE_NETS=9\nNATIVE_BD_PORT_MEMBERS=18\nPHYSICAL_TOP_PORTS=16\nPROTECTED_IPS=11\nPROTECTED_CONFIG_PROPERTIES=$config_count\nPROJECT_REFERENCES=$file_count\nORIGINAL_MAINLINE_REFERENCES=0\nDOWNLOADS_REFERENCES=0\nNATIVE_BD_VALIDATE=PASS\nSOURCE_TIMING=PENDING\nBOARD_ELECTRICAL=PENDING\nNOT_DOWNLOAD_READY"
    close $status
    puts "M2_EXTRACTED_PROJECT_REOPEN=PASS: $status_path"
} message options]} {
    set status [open $status_path w]
    puts $status "FAIL\nCOPY=$copy_root\n$message\n[dict get $options -errorinfo]"
    close $status
    puts "M2_EXTRACTED_PROJECT_REOPEN=FAIL: $message"
    puts [dict get $options -errorinfo]
    if {$reopened_project && [llength [current_project -quiet]]} {
        if {[string equal -nocase [file normalize [get_property DIRECTORY [current_project]]] $copy_root]} {
            catch {close_project}
        }
    }
    exit 1
}
exit 0
