`timescale 1ns/1ps
`default_nettype none
// Plain-Verilog module-reference boundary for the CURRENT radar_system BD.
// The existing AMD FIFO and SG DMA stay OUTSIDE this block and are reused.
// No synthetic source, no data-source MUX, no software implementation included.
module radar_capture_bd #(
    parameter integer SYS_CLOCK_HZ=100000000,
    parameter integer POWER_QUAL_CYCLES=100000,
    parameter integer RX_CLOCK_TIMEOUT=1000000,
    parameter integer FIRST_DATA_TIMEOUT=500000000
)(
    (* X_INTERFACE_INFO="xilinx.com:signal:clock:1.0 clk_sys CLK",
       X_INTERFACE_PARAMETER="XIL_INTERFACENAME clk_sys, ASSOCIATED_RESET rstn_sys" *)
    input wire clk_sys,
    (* X_INTERFACE_INFO="xilinx.com:signal:reset:1.0 rstn_sys RST",
       X_INTERFACE_PARAMETER="XIL_INTERFACENAME rstn_sys, POLARITY ACTIVE_LOW" *)
    input wire rstn_sys,
    (* X_INTERFACE_INFO="xilinx.com:signal:clock:1.0 clk_ref CLK",
       X_INTERFACE_PARAMETER="XIL_INTERFACENAME clk_ref, ASSOCIATED_RESET rst_ref" *)
    input wire clk_ref,
    (* X_INTERFACE_INFO="xilinx.com:signal:reset:1.0 rst_ref RST",
       X_INTERFACE_PARAMETER="XIL_INTERFACENAME rst_ref, POLARITY ACTIVE_HIGH" *)
    input wire rst_ref,
    input wire dma_stream_resetn,
    input wire [31:0] ctrl_i, output wire [31:0] status_o,
    input wire ps_uart_tx_i, output wire ps_uart_rx_o,
    input wire radar_uart_rx_raw, input wire radar_3v3_raw, input wire radar_nerr_n_raw,
    output wire radar_uart_tx_data, output wire radar_uart_tx_oe,
    output wire radar_nrst_low, output wire radar_sync_data, output wire radar_sync_oe,
    input wire bitclk_p, input wire bitclk_n, input wire frclk_p, input wire frclk_n,
    input wire data0_p, input wire data0_n, input wire data1_p, input wire data1_n,
    (* X_INTERFACE_INFO="xilinx.com:signal:clock:1.0 rx_clk_o CLK",
       X_INTERFACE_PARAMETER="XIL_INTERFACENAME rx_clk_o, FREQ_HZ 75000000, ASSOCIATED_BUSIF M_AXIS_CAPTURE, ASSOCIATED_RESET capture_resetn" *)
    output wire rx_clk_o,
    (* X_INTERFACE_INFO="xilinx.com:signal:reset:1.0 capture_resetn RST",
       X_INTERFACE_PARAMETER="XIL_INTERFACENAME capture_resetn, POLARITY ACTIVE_LOW" *)
    output wire capture_resetn,
    (* X_INTERFACE_INFO="xilinx.com:interface:axis:1.0 M_AXIS_CAPTURE TDATA" *)
    output wire [63:0] m_axis_tdata,
    (* X_INTERFACE_INFO="xilinx.com:interface:axis:1.0 M_AXIS_CAPTURE TKEEP" *)
    output wire [7:0] m_axis_tkeep,
    (* X_INTERFACE_INFO="xilinx.com:interface:axis:1.0 M_AXIS_CAPTURE TVALID" *)
    output wire m_axis_tvalid,
    (* X_INTERFACE_INFO="xilinx.com:interface:axis:1.0 M_AXIS_CAPTURE TREADY" *)
    input wire m_axis_tready,
    (* X_INTERFACE_INFO="xilinx.com:interface:axis:1.0 M_AXIS_CAPTURE TLAST" *)
    output wire m_axis_tlast,
    input wire [31:0] fifo_wr_count, input wire fifo_prog_full, input wire fifo_almost_full
);
    wire [3:0] io_cmd;
    wire pg, pg_qualified, nerr_n, power_lost, io_active;
    wire phy_ready, ref_ready, deser_resetn, delay_busy, delay_send;
    wire arm_admissible_sys, ref_cal_ready_sys, rx_ready_sys, baseline_valid_sys;
    wire tap_valid_rx;
    wire [26:0] baseline_data, baseline_readback_rx;
    wire [3:0] cfg_result, phy_fault_sys;
    wire [2:0] cfg_phase;
    wire [31:0] cfg_generation, baseline_generation_sys, tap_generation_rx;
    wire [27:0] delay_data;
    wire [26:0] delay_readback;
    wire [7:0] fr_byte, d0_byte, d1_byte;
    wire recover_active, abort_level, session_open, transaction_cancel;
    wire [15:0] sys_faults;
    // DMA reset and recovery are SYS-domain sources. Register each reset-tree
    // owner before destination reset synchronizers: no cross-domain combinational
    // reset fan-in. Assertion takes at most two SYS edges and needs no RX edge.
    reg common_resetn, command_resetn;
    always @(posedge clk_sys or negedge rstn_sys) begin
        if(!rstn_sys) begin common_resetn<=0; command_resetn<=0; end
        else begin
            common_resetn<=dma_stream_resetn && !recover_active;
            command_resetn<=dma_stream_resetn && !recover_active &&
                             !abort_level && !transaction_cancel;
        end
    end
    wire rx_control_resetn, rx_command_resetn, sys_control_resetn, sys_command_resetn;
    wire cmd_send, cmd_busy, cmd_rx_valid;
    wire [143:0] cmd_data, cmd_rx_data;
    wire [383:0] rx_snapshot, sys_snapshot;
    wire snapshot_req, snap_busy_rx, snap_valid, snap_busy_tap, tap_valid;
    wire [31:0] snap_gen,tap_gen;
    wire [63:0] tap_snapshot;
    wire rx_complete,rx_fault,rx_first,rx_pending,rx_idle;
    wire complete_sys,fault_sys,first_sys,pending_sys,idle_sys,fifo_ready_sys,deser_sys;
    wire heartbeat_sys;
    reg [7:0] heartbeat_counter;

    radar_io_guard #(.POWER_QUAL_CYCLES(POWER_QUAL_CYCLES)) u_io(
        .clk(clk_sys),.resetn(rstn_sys),.io_cmd(io_cmd),
        .pg_raw(radar_3v3_raw),.nerr_n_raw(radar_nerr_n_raw),.uart_rx_raw(radar_uart_rx_raw),
        .ps_uart_tx(ps_uart_tx_i),.ps_uart_rx(ps_uart_rx_o),
        .tx_data(radar_uart_tx_data),.tx_oe(radar_uart_tx_oe),.nrst_low(radar_nrst_low),
        .sync_data(radar_sync_data),.sync_oe(radar_sync_oe),
        .pg_sync(pg),.nerr_n_sync(nerr_n),.power_qualified(pg_qualified),
        .power_lost(power_lost),.io_active(io_active));
    radar_lvds_phy u_phy(
        .sys_clk(clk_sys),.sys_resetn(rstn_sys),.ref_clk(clk_ref),.ref_reset(rst_ref),
        .cfg_send(delay_send),.cfg_data(delay_data),.cfg_busy(delay_busy),.ready_sys(phy_ready),
        .cancel_sys(!common_resetn || abort_level || transaction_cancel),
        .arm_admissible_sys(arm_admissible_sys),.ref_cal_ready_sys(ref_cal_ready_sys),
        .rx_ready_sys(rx_ready_sys),.baseline_valid_sys(baseline_valid_sys),
        .baseline_data(baseline_data),.baseline_generation_sys(baseline_generation_sys),
        .cfg_result(cfg_result),.cfg_phase(cfg_phase),.phy_fault_sys(phy_fault_sys),
        .cfg_generation(cfg_generation),.tap_valid_rx(tap_valid_rx),
        .tap_generation_rx(tap_generation_rx),
        .baseline_readback_rx(baseline_readback_rx),
        .bitclk_p(bitclk_p),.bitclk_n(bitclk_n),.frclk_p(frclk_p),.frclk_n(frclk_n),
        .data0_p(data0_p),.data0_n(data0_n),.data1_p(data1_p),.data1_n(data1_n),
        .rx_clk(rx_clk_o),.deser_resetn(deser_resetn),
        .fr_byte(fr_byte),.data0_byte(d0_byte),.data1_byte(d1_byte),
        .delay_readback(delay_readback),.ref_ready(ref_ready));

    // Only the existing FIFO's ONE reset input is driven. Release after real
    // RX edges while the PS-side 100 MHz clock is already present. Neither a
    // stopped BITCLK nor the 300 MHz reference is synthesized as a fake RX clock.
    radar_reset_release #(.HOLD_CYCLES(1)) u_common_sys_release(
        .clk(clk_sys),.allow_n(common_resetn),.resetn(sys_control_resetn));
    radar_reset_release #(.HOLD_CYCLES(1)) u_command_sys_release(
        .clk(clk_sys),.allow_n(command_resetn),.resetn(sys_command_resetn));
    radar_reset_release #(.HOLD_CYCLES(1)) u_rx_control_release(
        .clk(rx_clk_o),.allow_n(common_resetn),.resetn(rx_control_resetn));
    // FIFO alone retains the longer local HOLD. Its input is the same RX
    // release used by snapshots/heartbeat, never another raw-reset XPM chain.
    radar_reset_release #(.HOLD_CYCLES(64),.PREQUALIFIED(1)) u_fifo_release(
        .clk(rx_clk_o),.allow_n(rx_control_resetn),.resetn(capture_resetn));
    radar_reset_release #(.HOLD_CYCLES(1)) u_command_release(
        .clk(rx_clk_o),.allow_n(command_resetn),.resetn(rx_command_resetn));
    always @(posedge rx_clk_o or negedge rx_control_resetn)
        if(!rx_control_resetn) heartbeat_counter<=0;
        else heartbeat_counter<=heartbeat_counter+1'b1;

    radar_mailbox #(.WIDTH(144),.EXTERNAL_SRC_RESET(1),.EXTERNAL_DST_RESET(1)) u_cmd_mb(
        .src_clk(clk_sys),.dst_clk(rx_clk_o),.arstn(command_resetn),
        .src_resetn_in(sys_command_resetn),.dst_resetn_in(rx_command_resetn),
        .src_send(cmd_send),.src_data(cmd_data),.src_busy(cmd_busy),.src_done(),
        .dst_valid(cmd_rx_valid),.dst_data(cmd_rx_data));
    radar_receive u_receive(
        .rx_clk(rx_clk_o),.common_resetn(rx_command_resetn),.stream_resetn(capture_resetn),
        .deser_resetn(deser_resetn),.cmd_valid(cmd_rx_valid),.cmd_data(cmd_rx_data),
        .abort_level(abort_level),.fr_byte(fr_byte),.data0_byte(d0_byte),.data1_byte(d1_byte),
        .fifo_count(fifo_wr_count),.fifo_prog_full(fifo_prog_full),.fifo_almost_full(fifo_almost_full),
        .tdata(m_axis_tdata),.tkeep(m_axis_tkeep),.tvalid(m_axis_tvalid),.tready(m_axis_tready),.tlast(m_axis_tlast),
        .snapshot(rx_snapshot),.complete(rx_complete),.fault(rx_fault),.first_seen(rx_first),
        .pending(rx_pending),.idle_observed(rx_idle));
    radar_snapshot #(.WIDTH(384),.EXTERNAL_SYS_RESET(1),.EXTERNAL_RX_RESET(1)) u_snapshot(
        .sys_clk(clk_sys),.rx_clk(rx_clk_o),.arstn(common_resetn),.request(snapshot_req),
        .sys_resetn_in(sys_control_resetn),.rx_resetn_in(rx_control_resetn),
        .sample_valid(1'b1),.rx_data(rx_snapshot),.busy(snap_busy_rx),.valid(snap_valid),.data(sys_snapshot),.generation(snap_gen));
    // Copy the measurement generation atomically with its actual RX readback.
    // tap_gen remains a transport-copy counter, not a new measurement claim.
    radar_snapshot #(.WIDTH(64),.EXTERNAL_SYS_RESET(1),.EXTERNAL_RX_RESET(1)) u_tap_snapshot(
        .sys_clk(clk_sys),.rx_clk(rx_clk_o),.arstn(common_resetn),.request(snapshot_req),
        .sys_resetn_in(sys_control_resetn),.rx_resetn_in(rx_control_resetn),
        .sample_valid(tap_valid_rx),
        .rx_data({tap_generation_rx,5'd0,delay_readback}),.busy(snap_busy_tap),.valid(tap_valid),.data(tap_snapshot),.generation(tap_gen));
    radar_sync_bit u_complete(.clk(clk_sys),.arstn(common_resetn),.din(rx_complete),.dout(complete_sys));
    radar_sync_bit u_fault(.clk(clk_sys),.arstn(common_resetn),.din(rx_fault),.dout(fault_sys));
    radar_sync_bit u_first(.clk(clk_sys),.arstn(common_resetn),.din(rx_first),.dout(first_sys));
    radar_sync_bit u_pending(.clk(clk_sys),.arstn(common_resetn),.din(rx_pending),.dout(pending_sys));
    radar_sync_bit u_idle(.clk(clk_sys),.arstn(common_resetn),.din(rx_idle),.dout(idle_sys));
    radar_sync_bit u_fifo_ready(.clk(clk_sys),.arstn(common_resetn),.din(capture_resetn),.dout(fifo_ready_sys));
    radar_sync_bit u_deser_ready(.clk(clk_sys),.arstn(rstn_sys),.din(deser_resetn),.dout(deser_sys));
    radar_sync_bit u_heartbeat(.clk(clk_sys),.arstn(common_resetn),.din(heartbeat_counter[7]),.dout(heartbeat_sys));
    radar_control #(.SYS_CLOCK_HZ(SYS_CLOCK_HZ),.RX_CLOCK_TIMEOUT(RX_CLOCK_TIMEOUT),
                    .FIRST_DATA_TIMEOUT(FIRST_DATA_TIMEOUT)) u_control(
        .clk(clk_sys),.resetn(rstn_sys),.ctrl_i(ctrl_i),.status_o(status_o),.io_cmd(io_cmd),
        .pg(pg),.pg_qualified(pg_qualified),.nerr_n(nerr_n),.power_lost(power_lost),
        .io_active(io_active),.tx_oe(io_active && io_cmd[1] && !io_cmd[2]),
        .nrst_low(io_active && io_cmd[2]),.sync_oe(io_active && !io_cmd[2]),
        .arm_admissible_sys(arm_admissible_sys),.ref_cal_ready_sys(ref_cal_ready_sys),
        .rx_ready_sys(rx_ready_sys),.baseline_valid_sys(baseline_valid_sys),
        .baseline_data(baseline_data),.baseline_generation_sys(baseline_generation_sys),
        .cfg_result(cfg_result),.cfg_phase(cfg_phase),.phy_fault_sys(phy_fault_sys),
        .cfg_generation(cfg_generation),.deser_ready(deser_sys),.fifo_ready(fifo_ready_sys),
        .dma_stream_resetn(dma_stream_resetn),.rx_heartbeat(heartbeat_sys),
        .rx_complete(complete_sys),.rx_fault(fault_sys),.rx_first(first_sys),
        .rx_pending(pending_sys),.rx_idle(idle_sys),
        .cmd_busy(cmd_busy),.cmd_send(cmd_send),.cmd_data(cmd_data),
        .snapshot_busy(snap_busy_rx || snap_busy_tap),.snapshot_valid(snap_valid),
        .snapshot_data(sys_snapshot),.snapshot_generation(snap_gen),.snapshot_request(snapshot_req),
        .tap_snapshot_valid(tap_valid),.tap_snapshot_data(tap_snapshot),.tap_snapshot_generation(tap_gen),
        .delay_busy(delay_busy),.delay_send(delay_send),.delay_data(delay_data),
        .recover_active(recover_active),.abort_level(abort_level),.transaction_cancel(transaction_cancel),
        .session_open(session_open),.sys_faults(sys_faults));
endmodule
`default_nettype wire
