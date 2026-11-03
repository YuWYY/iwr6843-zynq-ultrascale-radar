source [file join [file dirname [info script]] common.tcl]
namespace eval m2real {
    variable wires {
        zynq_ultra_ps_e_0/pl_clk0 radar_capture_bd_0/clk_sys
        rst_sys_100m/peripheral_aresetn radar_capture_bd_0/rstn_sys
        clk_wiz_phy_ref/clk_out1 radar_capture_bd_0/clk_ref
        rst_phy_ref_300m/peripheral_reset radar_capture_bd_0/rst_ref
        axi_dma_capture/s2mm_prmry_reset_out_n radar_capture_bd_0/dma_stream_resetn
        axi_gpio_ctrl/gpio_io_o radar_capture_bd_0/ctrl_i
        radar_capture_bd_0/status_o axi_gpio_ctrl/gpio2_io_i
        zynq_ultra_ps_e_0/emio_uart1_txd radar_capture_bd_0/ps_uart_tx_i
        radar_capture_bd_0/ps_uart_rx_o zynq_ultra_ps_e_0/emio_uart1_rxd
        radar_capture_bd_0/rx_clk_o axis_fifo_capture/s_axis_aclk
        radar_capture_bd_0/capture_resetn axis_fifo_capture/s_axis_aresetn
        axis_fifo_capture/axis_wr_data_count radar_capture_bd_0/fifo_wr_count
        axis_fifo_capture/prog_full radar_capture_bd_0/fifo_prog_full
        axis_fifo_capture/almost_full radar_capture_bd_0/fifo_almost_full
    }
    variable ports {
        bitclk_p radar_bitclk_p I bitclk_n radar_bitclk_n I
        frclk_p radar_frclk_p I frclk_n radar_frclk_n I
        data0_p radar_data0_p I data0_n radar_data0_n I
        data1_p radar_data1_p I data1_n radar_data1_n I
        radar_uart_rx_raw radar_uart_rx_raw I radar_3v3_raw radar_3v3_raw I
        radar_nerr_n_raw radar_nerr_n_raw I
        radar_uart_tx_data radar_uart_tx_data O radar_uart_tx_oe radar_uart_tx_oe O
        radar_nrst_low radar_nrst_low O radar_sync_data radar_sync_data O radar_sync_oe radar_sync_oe O
    }
    proc connect {a b} {
        foreach p [list $a $b] {require {[llength [get_bd_pins -quiet $p]]==1} "Missing pin $p"}
        require {[get_property DIR [get_bd_pins $a]] eq "O" && [get_property DIR [get_bd_pins $b]] eq "I"} "Wrong direction $a -> $b"
        require {[width $a]==[width $b]} "Width mismatch $a -> $b"
        set na [get_bd_nets -quiet -of_objects [get_bd_pins $a]]
        set nb [get_bd_nets -quiet -of_objects [get_bd_pins $b]]
        if {[llength $na] && $na eq $nb} {return}
        if {[llength $nb]} {error "Refusing to replace occupied input $b on $nb"}
        connect_bd_net [get_bd_pins $a] [get_bd_pins $b]
    }
    proc integrate {} {
        variable root; variable wires; variable ports; variable delivery
        require {[llength [get_bd_cells -quiet -filter {VLNV =~ "*:module_ref:radar_capture_bd:*"}]]==1} {Expected exactly one capture module}
        require {[llength [get_bd_cells -quiet radar_capture_bd_0]]==1} {Unexpected capture instance name}
        require {[llength [get_bd_cells -quiet -filter {VLNV =~ "*:axi_gpio:*"}]]==1} {Expected one GPIO}
        require {[llength [get_bd_cells -quiet f20_radar_m1_bd_0]]==0} {Unexpected legacy BD module}
        set rtl [glob -directory [file join $root rtl] */*.v]
        require {[llength $rtl]==13} {Expected thirteen production RTL files}
        foreach f $rtl {
            require {[llength [get_files -quiet $f]]==1} "Missing or duplicate source $f"
            set_property FILE_TYPE Verilog [get_files $f]
        }
        update_compile_order -fileset sources_1
        set module_ip [get_property CONFIG.Component_Name [get_bd_cells radar_capture_bd_0]]
        require {[llength [get_ips -quiet $module_ip]]==1} "Missing native module-reference IP $module_ip"
        update_module_reference [get_ips $module_ip]
        foreach {a b} $wires {connect $a $b}
        set mn [get_bd_intf_nets -quiet -of_objects [get_bd_intf_pins radar_capture_bd_0/M_AXIS_CAPTURE]]
        set sn [get_bd_intf_nets -quiet -of_objects [get_bd_intf_pins axis_fifo_capture/S_AXIS]]
        if {![llength $mn] && ![llength $sn]} {
            connect_bd_intf_net [get_bd_intf_pins radar_capture_bd_0/M_AXIS_CAPTURE] [get_bd_intf_pins axis_fifo_capture/S_AXIS]
        } elseif {$mn ne $sn || ![llength $mn]} {error {Occupied or mismatched capture AXIS interface}}
        foreach {pin port dir} $ports {
            if {![llength [get_bd_ports -quiet $port]]} {create_bd_port -dir $dir $port}
            require {[get_property DIR [get_bd_ports $port]] eq $dir} "Wrong port direction $port"
            set a radar_capture_bd_0/$pin
            set n [get_bd_nets -quiet -of_objects [get_bd_pins $a]]
            set pn [get_bd_nets -quiet -of_objects [get_bd_ports $port]]
            if {[llength $n] && $n eq $pn} {continue}
            require {![llength $n] && ![llength $pn]} "Occupied external connection $port"
            connect_bd_net [get_bd_pins $a] [get_bd_ports $port]
        }
        require {[llength [get_bd_intf_ports phy_ref_clk_100m]]==1} {Missing reference clock}
        foreach rel {
            common/cdc_bit_sync.sv common/heartbeat.sv control/radar_sideband_ctrl.sv
            demo/axis_counter_source.sv demo/axis_stream_metrics.sv demo/radar_demo_datapath.sv
            top/f20_radar_pl_core.sv top/f20_radar_m1_bd.v
        } {
            set p [file join $root Radar_XCZU2CG_M2.srcs sources_1 imports rtl $rel]
            if {[llength [get_files -quiet $p]]} {remove_files [get_files $p]}
        }
        foreach r {synth_1 impl_1} {
            set_property AUTO_INCREMENTAL_CHECKPOINT 0 [get_runs $r]
            set_property INCREMENTAL_CHECKPOINT {} [get_runs $r]
        }
        set olddcp [file join $root Radar_XCZU2CG_M2.srcs utils_1 imports synth_1 radar_system_wrapper.dcp]
        if {[llength [get_files -quiet $olddcp]]} {remove_files [get_files $olddcp]}
        foreach r {clk_wiz_sys_impl_1 clk_wiz_sys_synth_1} {
            if {[llength [get_runs -quiet $r]]} {delete_runs [get_runs $r]}
        }
        set oldip [file join $root Radar_XCZU2CG_M2.srcs sources_1 ip clk_wiz_sys clk_wiz_sys.xci]
        if {[llength [get_files -quiet -all $oldip]]} {remove_files [get_files -all $oldip]}
        if {[llength [get_filesets -quiet clk_wiz_sys]]} {delete_fileset [get_filesets clk_wiz_sys]}
        # Removing a BlockSrcs set can move its XCI back to sources_1.
        if {[llength [get_files -quiet -all $oldip]]} {remove_files [get_files -all $oldip]}
        require {![llength [get_files -quiet -all $oldip]]} {Legacy Clock Wizard still registered}
        foreach name {radar_pins.xdc radar_clocks.xdc radar_cdc.xdc} {
            set path [file join $root constraints $name]
            if {![llength [get_files -quiet $path]]} {add_files -fileset constrs_1 -norecurse $path}
            if {$name eq "radar_cdc.xdc"} {
                set_property PROCESSING_ORDER LATE [get_files $path]
                set_property USED_IN_SYNTHESIS false [get_files $path]
            }
        }
        foreach path [glob -nocomplain -directory [file join $root sim] *.sv] {
            if {![llength [get_files -quiet $path]]} {add_files -fileset sim_1 -norecurse $path}
        }
        set_property TOP_AUTO_SET false [get_filesets sim_1]
        set_property TOP tb_capture_fifo [get_filesets sim_1]
        save_bd_design
        puts {M2_WIRED_AND_LEGACY_REFERENCES_REMOVED}
    }
}
m2real::integrate
