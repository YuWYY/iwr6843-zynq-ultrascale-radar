`timescale 1ns/1ps
`default_nettype none
// UltraScale+ RX PHY: REF supervises calibration, RX executes delay updates,
// SYS owns transactions and makes cancellation survive a stopped sensor clock.
module radar_lvds_phy #(
    parameter integer DELAY_TIMEOUT_SYS_CYCLES=10000,
    parameter integer CAL_TIMEOUT_SYS_CYCLES=120000,
    parameter integer RX_ACTIVITY_TIMEOUT=128,
    parameter integer CAL_RDY_TIMEOUT_CYCLES=300000
)(
    input wire sys_clk, input wire sys_resetn,
    input wire ref_clk, input wire ref_reset,
    input wire cfg_send, input wire [27:0] cfg_data, input wire cancel_sys,
    output reg cfg_busy, output wire ready_sys,
    output reg arm_admissible_sys, output wire ref_cal_ready_sys,
    output reg rx_ready_sys, output reg baseline_valid_sys,
    output reg [26:0] baseline_data, output reg [31:0] baseline_generation_sys,
    output reg [3:0] cfg_result, output reg [3:0] phy_fault_sys,
    output reg [2:0] cfg_phase,
    output reg [31:0] cfg_generation,
    input wire bitclk_p, input wire bitclk_n,
    input wire frclk_p, input wire frclk_n,
    input wire data0_p, input wire data0_n,
    input wire data1_p, input wire data1_n,
    output wire rx_clk, output wire deser_resetn,
    output wire [7:0] fr_byte, output wire [7:0] data0_byte, output wire [7:0] data1_byte,
    output wire [26:0] delay_readback, output wire [26:0] baseline_readback_rx,
    output wire tap_valid_rx, output wire [31:0] tap_generation_rx,
    output wire ref_ready
);
    localparam SUCCESS=4'd1, NOT_READY=4'd2, BELOW_BASELINE=4'd3,
        RDY_LOST=4'd6, TIMEOUT=4'd7, CANCELED=4'd8, CALIBRATION_FAILED=4'd9;
    localparam ACCEPTED=3'd1, DELIVERED=3'd2, EXECUTING=3'd3,
        SUCCEEDED=3'd4, FAILED=3'd5, CANCELLED=3'd6;
    wire phy_sys_resetn, ref_resetn, rx_resetn, ref_reset_sys_n;
    wire rx_epoch_sys_resetn, cal_epoch_sys_resetn;
    reg phy_allow_sys;
    wire bit_clk_in, bit_clk_global, serdes_clk, bitclk_resetn, activity_resetn;
    wire [2:0] serial_in, delayed, delay_load;
    wire ctl_reset, primitive_reset, clock_enable_ref, en_vtc, idelay_rdy;
    wire cal_enable_ref, gate_open_ref;
    wire [26:0] tap_values, tap_count_live;
    wire [3:0] delay_state;
    wire rx_ready, rx_executing, rx_baseline_valid, cal_executing;
    wire cal_ready_rx, rx_ready_sync, rx_executing_sys, cal_executing_sys;
    reg rx_epoch_resetn, cal_epoch_resetn;
    reg initialized, need_recal, ref_seen, owner_recal;
    reg dispatch_pending, send_delay, send_recal;
    reg [26:0] target_hold;
    reg [31:0] transaction_age;
    reg [3:0] dispatch_age, cancel_age;
    reg cancel_pending;
    reg [3:0] cancel_result;
    wire delay_mb_busy, delay_delivered, delay_valid;
    wire [26:0] delay_target;
    wire recal_mb_busy, recal_delivered, recal_valid;
    wire recal_unused;
    wire rx_report_send, rx_report_busy, rx_report_valid;
    wire [58:0] rx_report_data, rx_report_received;
    wire cal_report_send, cal_report_busy, cal_report_valid;
    wire cal_report_success, cal_report_received;
    wire heartbeat_sys;
    reg [3:0] heartbeat_counter;
    reg heartbeat_seen, clock_alive;
    reg [31:0] clock_age;

    // Isolate the asynchronous REF source before combining reset ownership.
    // REF withdrawal reaches the owner within one running SYS edge. SYS reset
    // itself can still assert safety with SYS, REF and BITCLK all stopped.
    radar_reset_release #(.HOLD_CYCLES(1)) u_ref_reset_sys_release(
        .clk(sys_clk), .allow_n(!ref_reset), .resetn(ref_reset_sys_n));
    always @(posedge sys_clk or negedge sys_resetn) begin
        if(!sys_resetn) phy_allow_sys<=0;
        else phy_allow_sys<=ref_reset_sys_n;
    end
    radar_reset_release #(.HOLD_CYCLES(1)) u_phy_sys_release(
        .clk(sys_clk), .allow_n(phy_allow_sys), .resetn(phy_sys_resetn));
    radar_reset_release #(.HOLD_CYCLES(1)) u_ref_release(
        .clk(ref_clk), .allow_n(phy_sys_resetn), .resetn(ref_resetn));
    radar_reset_release #(.HOLD_CYCLES(1)) u_rx_release(
        .clk(rx_clk), .allow_n(rx_epoch_resetn), .resetn(rx_resetn));
    radar_reset_release #(.HOLD_CYCLES(1)) u_rx_epoch_sys_release(
        .clk(sys_clk), .allow_n(rx_epoch_resetn), .resetn(rx_epoch_sys_resetn));
    radar_reset_release #(.HOLD_CYCLES(1)) u_cal_epoch_sys_release(
        .clk(sys_clk), .allow_n(cal_epoch_resetn), .resetn(cal_epoch_sys_resetn));
    // One REF epoch qualification is shared by the supervisor and both
    // mailbox endpoints. Async assertion remembers cancellation while REF is
    // stopped; resuming REF first clears the old supervisor request/report.
    // Its global reset remains ref_resetn so QUIESCE itself can keep running.
    radar_reset_release #(.HOLD_CYCLES(1)) u_cal_epoch_ref_release(
        .clk(ref_clk), .allow_n(cal_epoch_resetn), .resetn(cal_enable_ref));
    radar_mailbox #(.WIDTH(27),.EXTERNAL_SRC_RESET(1),.EXTERNAL_DST_RESET(1)) u_delay_mb(
        .src_clk(sys_clk),.dst_clk(rx_clk),.arstn(rx_epoch_resetn),
        .src_resetn_in(rx_epoch_sys_resetn),.dst_resetn_in(rx_resetn),
        .src_send(send_delay),.src_data(target_hold),.src_busy(delay_mb_busy),
        .src_done(delay_delivered),.dst_valid(delay_valid),.dst_data(delay_target));
    radar_mailbox #(.WIDTH(59),.EXTERNAL_SRC_RESET(1),.EXTERNAL_DST_RESET(1)) u_delay_report_mb(
        .src_clk(rx_clk),.dst_clk(sys_clk),.arstn(rx_epoch_resetn),
        .src_resetn_in(rx_resetn),.dst_resetn_in(rx_epoch_sys_resetn),
        .src_send(rx_report_send),.src_data(rx_report_data),.src_busy(rx_report_busy),
        .src_done(),.dst_valid(rx_report_valid),.dst_data(rx_report_received));
    radar_mailbox #(.WIDTH(1),.EXTERNAL_SRC_RESET(1),.EXTERNAL_DST_RESET(1)) u_recal_mb(
        .src_clk(sys_clk),.dst_clk(ref_clk),.arstn(cal_epoch_resetn),
        .src_resetn_in(cal_epoch_sys_resetn),.dst_resetn_in(cal_enable_ref),
        .src_send(send_recal),.src_data(1'b1),.src_busy(recal_mb_busy),
        .src_done(recal_delivered),.dst_valid(recal_valid),.dst_data(recal_unused));
    radar_mailbox #(.WIDTH(1),.EXTERNAL_SRC_RESET(1),.EXTERNAL_DST_RESET(1)) u_cal_report_mb(
        .src_clk(ref_clk),.dst_clk(sys_clk),.arstn(cal_epoch_resetn),
        .src_resetn_in(cal_enable_ref),.dst_resetn_in(cal_epoch_sys_resetn),
        .src_send(cal_report_send),.src_data(cal_report_success),.src_busy(cal_report_busy),
        .src_done(),.dst_valid(cal_report_valid),.dst_data(cal_report_received));
    radar_phy_cal #(.RDY_TIMEOUT_CYCLES(CAL_RDY_TIMEOUT_CYCLES)) u_cal_ctrl(
        .clk_ref(ref_clk),.resetn_ref(ref_resetn),.idelay_rdy(idelay_rdy),
        .cal_enable(cal_enable_ref),.gate_open_ref(gate_open_ref),
        .recal_valid(recal_valid),.report_busy(cal_report_busy),
        .idelay_reset(ctl_reset),.primitive_reset(primitive_reset),
        .clock_enable_ref(clock_enable_ref),.cal_ready(ref_ready),.executing(cal_executing),
        .report_send(cal_report_send),.report_success(cal_report_success));
    radar_sync_bit u_gate_open_ref(.clk(ref_clk),.arstn(ref_resetn),
        .din(bitclk_resetn),.dout(gate_open_ref));
    radar_sync_bit u_cal_ready_rx(.clk(rx_clk),.arstn(rx_resetn),.din(ref_ready),.dout(cal_ready_rx));
    radar_phy_delay u_delay_ctrl(
        .clk_rx(rx_clk),.resetn_rx(rx_resetn),.observation_resetn(activity_resetn),.cal_ready(cal_ready_rx),
        .cfg_valid(delay_valid),.cfg_target(delay_target),.tap_count_live(tap_count_live),
        .report_busy(rx_report_busy),.en_vtc(en_vtc),.load(delay_load),.tap_values(tap_values),
        .ready(rx_ready),.executing(rx_executing),.baseline_valid(rx_baseline_valid),
        .baseline(baseline_readback_rx),.readback(delay_readback),
        .observation_generation(tap_generation_rx),.report_send(rx_report_send),
        .report_data(rx_report_data),.state(delay_state));
    assign tap_valid_rx=rx_ready;
    radar_sync_bit u_ready_sync(.clk(sys_clk),.arstn(phy_sys_resetn),.din(rx_ready),.dout(rx_ready_sync));
    radar_sync_bit u_ref_ready_sync(.clk(sys_clk),.arstn(phy_sys_resetn),.din(ref_ready),.dout(ref_cal_ready_sys));
    radar_sync_bit u_rx_exec_sync(.clk(sys_clk),.arstn(phy_sys_resetn),.din(rx_executing),.dout(rx_executing_sys));
    radar_sync_bit u_cal_exec_sync(.clk(sys_clk),.arstn(phy_sys_resetn),.din(cal_executing),.dout(cal_executing_sys));
    assign ready_sys=rx_ready_sys;

    // A genuine RX heartbeat is independent of the delay transaction epoch.
    radar_reset_release #(.HOLD_CYCLES(1)) u_activity_release(
        .clk(rx_clk),.allow_n(phy_sys_resetn),.resetn(activity_resetn));
    always @(posedge rx_clk or negedge activity_resetn)
        if(!activity_resetn) heartbeat_counter<=0;
        else heartbeat_counter<=heartbeat_counter+1'b1;
    radar_sync_bit u_activity_sync(.clk(sys_clk),.arstn(phy_sys_resetn),
        .din(heartbeat_counter[2]),.dout(heartbeat_sys));

    task complete;
        input [3:0] result;
        begin
            cfg_busy<=0; cfg_result<=result; cfg_generation<=cfg_generation+1'b1;
            cfg_phase<=(result==SUCCESS) ? SUCCEEDED : ((result==CANCELED) ? CANCELLED : FAILED);
            dispatch_pending<=0;
        end
    endtask
    task isolate;
        input [3:0] result;
        begin
            // Both request and result endpoints share these persistent resets.
            // Eight SYS clocks of isolation precede ownership release.
            rx_epoch_resetn<=0; cal_epoch_resetn<=0; need_recal<=1;
            phy_fault_sys<=result;
            baseline_valid_sys<=0; rx_ready_sys<=0; arm_admissible_sys<=0;
            send_delay<=0; send_recal<=0; dispatch_pending<=0; ref_seen<=0;
            if(cfg_busy) begin
                // Cancellation wins even if complete() ran earlier this same
                // edge: retain ownership and undo that speculative terminal.
                cfg_busy<=1; cfg_generation<=cfg_generation;
                cfg_phase<=cfg_phase; cfg_result<=0;
                cancel_pending<=1; cancel_age<=0; cancel_result<=result;
            end
        end
    endtask
    always @(posedge sys_clk or negedge phy_sys_resetn) begin
        if(!phy_sys_resetn) begin
            cfg_busy<=0; cfg_result<=0; cfg_phase<=0; cfg_generation<=0;
            phy_fault_sys<=0;
            arm_admissible_sys<=0; rx_ready_sys<=0; baseline_valid_sys<=0;
            baseline_data<=0; baseline_generation_sys<=0;
            rx_epoch_resetn<=0; cal_epoch_resetn<=0; initialized<=0;
            need_recal<=0; ref_seen<=0; owner_recal<=0;
            dispatch_pending<=0; send_delay<=0; send_recal<=0; target_hold<=0;
            transaction_age<=0; dispatch_age<=0; cancel_age<=0;
            cancel_pending<=0; cancel_result<=0;
            heartbeat_seen<=0; clock_alive<=0; clock_age<=32'hffffffff;
        end else begin
            send_delay<=0; send_recal<=0;
            heartbeat_seen<=heartbeat_sys;
            if(heartbeat_sys!=heartbeat_seen) begin clock_age<=0; clock_alive<=1; end
            else begin
                if(clock_age!=32'hffffffff) clock_age<=clock_age+1'b1;
                if(clock_age>=RX_ACTIVITY_TIMEOUT-1) clock_alive<=0;
            end
            arm_admissible_sys<=ref_cal_ready_sys && !need_recal && !cfg_busy && !cancel_sys;
            rx_ready_sys<=rx_ready_sync && baseline_valid_sys && rx_epoch_resetn &&
                          !need_recal && !cfg_busy && clock_alive;
            if(!initialized) begin initialized<=1; cal_epoch_resetn<=1; end
            if(ref_cal_ready_sys && !need_recal && !cfg_busy) begin
                ref_seen<=1; rx_epoch_resetn<=1;
            end
            if(rx_report_valid && !rx_report_received[58]) begin
                if(rx_report_received[57:54]==SUCCESS && rx_epoch_resetn && !need_recal) begin
                    baseline_data<=rx_report_received[53:27]; baseline_valid_sys<=1;
                    baseline_generation_sys<=baseline_generation_sys+1'b1;
                end else if(rx_epoch_resetn) isolate(rx_report_received[57:54]);
            end
            // Automatic boot calibration has no software owner/generation.
            // Record its failure independently; a new RECAL starts a fresh
            // reset epoch and deliberately supersedes the old boot report.
            if(cal_report_valid && !cal_report_received && !cfg_busy &&
               !(cfg_send && cfg_data[27])) isolate(CALIBRATION_FAILED);
            if(cancel_pending) begin
                if(cancel_age==7) begin
                    cancel_pending<=0; complete(cancel_result);
                end else cancel_age<=cancel_age+1'b1;
            end else if(cfg_busy) begin
                transaction_age<=transaction_age+1'b1;
                if(dispatch_pending) begin
                    if(dispatch_age!=15) dispatch_age<=dispatch_age+1'b1;
                    if(owner_recal && dispatch_age==7) cal_epoch_resetn<=1;
                    if(owner_recal && dispatch_age>=8 && !recal_mb_busy) begin
                        send_recal<=1; dispatch_pending<=0;
                    end else if(!owner_recal && !delay_mb_busy) begin
                        send_delay<=1; dispatch_pending<=0;
                    end
                end
                if((owner_recal && recal_delivered) || (!owner_recal && delay_delivered))
                    if(cfg_phase==ACCEPTED) cfg_phase<=DELIVERED;
                if((owner_recal && cal_executing_sys) || (!owner_recal && rx_executing_sys))
                    if(cfg_phase<=DELIVERED) cfg_phase<=EXECUTING;
                if(owner_recal && cal_report_valid) begin
                    if(cal_report_received) begin
                        need_recal<=0; ref_seen<=1; rx_epoch_resetn<=1;
                        complete(SUCCESS);
                    end else isolate(CALIBRATION_FAILED);
                end else if(!owner_recal && rx_report_valid && rx_report_received[58]) begin
                    if(rx_report_received[57:54]==SUCCESS || rx_report_received[57:54]==BELOW_BASELINE)
                        complete(rx_report_received[57:54]);
                    else isolate(rx_report_received[57:54]);
                end
                if((!owner_recal && (transaction_age>=DELAY_TIMEOUT_SYS_CYCLES-1 || !clock_alive)) ||
                   (owner_recal && transaction_age>=CAL_TIMEOUT_SYS_CYCLES-1)) isolate(TIMEOUT);
                if(cancel_sys) isolate(CANCELED);
            end else if(cfg_send) begin
                cfg_result<=0; cfg_phase<=ACCEPTED;
                if(cancel_sys) complete(CANCELED);
                else if(cfg_data[27]) begin
                    phy_fault_sys<=0;
                    cfg_busy<=1; owner_recal<=1; dispatch_pending<=1; dispatch_age<=0;
                    // Flush both REF request/result endpoints before allowing
                    // the new request. A delayed boot-failure report cannot
                    // terminate this new software owner.
                    transaction_age<=0; rx_epoch_resetn<=0; cal_epoch_resetn<=0;
                    baseline_valid_sys<=0; rx_ready_sys<=0; arm_admissible_sys<=0;
                    need_recal<=1; ref_seen<=0;
                end else if(!rx_ready_sys || !baseline_valid_sys || !clock_alive || need_recal)
                    complete(NOT_READY);
                else begin
                    cfg_busy<=1; owner_recal<=0; target_hold<=cfg_data[26:0];
                    dispatch_pending<=1; dispatch_age<=0; transaction_age<=0;
                    rx_ready_sys<=0; arm_admissible_sys<=0;
                end
            end
            // Unexpected calibration loss wins over ordinary command success.
            if(ref_seen && !ref_cal_ready_sys && !(cfg_busy && owner_recal) &&
               !(cfg_send && cfg_data[27]) && !cancel_pending) isolate(RDY_LOST);
        end
    end

    IBUFDS #(.IBUF_LOW_PWR("FALSE")) u_bclk_ibuf(.I(bitclk_p),.IB(bitclk_n),.O(bit_clk_in));
    BUFGCE #(.CE_TYPE("SYNC"),.IS_CE_INVERTED(1'b0),.IS_I_INVERTED(1'b0),
        .SIM_DEVICE("ULTRASCALE_PLUS")) u_bitclk_buf(.I(bit_clk_in),.CE(1'b1),.O(bit_clk_global));
    // Both primitive clock outputs are held LOW while asynchronous RST changes.
    // Their common CLR release uses the independent full BITCLK.
    radar_reset_release #(.HOLD_CYCLES(4)) u_bitclk_release(
        .clk(bit_clk_global),.allow_n(clock_enable_ref),.resetn(bitclk_resetn));
    BUFGCE_DIV #(.BUFGCE_DIVIDE(1),.IS_CE_INVERTED(1'b0),.IS_CLR_INVERTED(1'b0),
        .IS_I_INVERTED(1'b0),.SIM_DEVICE("ULTRASCALE_PLUS")) u_serdes_div(
        .I(bit_clk_in),.CE(1'b1),.CLR(!bitclk_resetn),.O(serdes_clk));
    BUFGCE_DIV #(.BUFGCE_DIVIDE(4),.IS_CE_INVERTED(1'b0),.IS_CLR_INVERTED(1'b0),
        .IS_I_INVERTED(1'b0),.SIM_DEVICE("ULTRASCALE_PLUS")) u_rx_div(
        .I(bit_clk_in),.CE(1'b1),.CLR(!bitclk_resetn),.O(rx_clk));
    IBUFDS #(.IBUF_LOW_PWR("FALSE")) u_fr_ibuf(.I(frclk_p),.IB(frclk_n),.O(serial_in[0]));
    IBUFDS #(.IBUF_LOW_PWR("FALSE")) u_d0_ibuf(.I(data0_p),.IB(data0_n),.O(serial_in[1]));
    IBUFDS #(.IBUF_LOW_PWR("FALSE")) u_d1_ibuf(.I(data1_p),.IB(data1_n),.O(serial_in[2]));
    (* IODELAY_GROUP="RADAR_BANK65" *) IDELAYCTRL #(.SIM_DEVICE("ULTRASCALE")) u_idelayctrl(
        .REFCLK(ref_clk),.RST(ctl_reset),.RDY(idelay_rdy));
    genvar n;
    generate for(n=0;n<3;n=n+1) begin: g_lane
        (* IODELAY_GROUP="RADAR_BANK65" *)
        IDELAYE3 #(.CASCADE("NONE"),.DELAY_FORMAT("TIME"),.DELAY_SRC("IDATAIN"),
            .DELAY_TYPE("VAR_LOAD"),.DELAY_VALUE(0),.IS_CLK_INVERTED(1'b0),
            .IS_RST_INVERTED(1'b0),.REFCLK_FREQUENCY(300.0),
            .SIM_DEVICE("ULTRASCALE_PLUS"),.UPDATE_MODE("ASYNC")) u_delay(
            .CASC_OUT(),.CNTVALUEOUT(tap_count_live[n*9 +: 9]),.DATAOUT(delayed[n]),
            .CASC_IN(1'b0),.CASC_RETURN(1'b0),.CE(1'b0),.CLK(rx_clk),
            .CNTVALUEIN(tap_values[n*9 +: 9]),.DATAIN(1'b0),.EN_VTC(en_vtc),
            .IDATAIN(serial_in[n]),.INC(1'b0),.LOAD(delay_load[n]),.RST(primitive_reset));
    end endgenerate
    radar_reset_release #(.HOLD_CYCLES(64),.PREQUALIFIED(1)) u_deser_release(
        .clk(rx_clk),.allow_n(rx_resetn && rx_ready),.resetn(deser_resetn));
    ISERDESE3 #(.DATA_WIDTH(8),.FIFO_ENABLE("FALSE"),.FIFO_SYNC_MODE("FALSE"),
        .IS_CLK_B_INVERTED(1'b1),.IS_CLK_INVERTED(1'b0),.IS_RST_INVERTED(1'b0),
        .SIM_DEVICE("ULTRASCALE_PLUS")) u_fr_ser(
        .Q(fr_byte),.FIFO_EMPTY(),.INTERNAL_DIVCLK(),.CLK(serdes_clk),
        .CLK_B(serdes_clk),.CLKDIV(rx_clk),.D(delayed[0]),.FIFO_RD_CLK(1'b0),
        .FIFO_RD_EN(1'b0),.RST(primitive_reset));
    ISERDESE3 #(.DATA_WIDTH(8),.FIFO_ENABLE("FALSE"),.FIFO_SYNC_MODE("FALSE"),
        .IS_CLK_B_INVERTED(1'b1),.IS_CLK_INVERTED(1'b0),.IS_RST_INVERTED(1'b0),
        .SIM_DEVICE("ULTRASCALE_PLUS")) u_d0_ser(
        .Q(data0_byte),.FIFO_EMPTY(),.INTERNAL_DIVCLK(),.CLK(serdes_clk),
        .CLK_B(serdes_clk),.CLKDIV(rx_clk),.D(delayed[1]),.FIFO_RD_CLK(1'b0),
        .FIFO_RD_EN(1'b0),.RST(primitive_reset));
    ISERDESE3 #(.DATA_WIDTH(8),.FIFO_ENABLE("FALSE"),.FIFO_SYNC_MODE("FALSE"),
        .IS_CLK_B_INVERTED(1'b1),.IS_CLK_INVERTED(1'b0),.IS_RST_INVERTED(1'b0),
        .SIM_DEVICE("ULTRASCALE_PLUS")) u_d1_ser(
        .Q(data1_byte),.FIFO_EMPTY(),.INTERNAL_DIVCLK(),.CLK(serdes_clk),
        .CLK_B(serdes_clk),.CLKDIV(rx_clk),.D(delayed[2]),.FIFO_RD_CLK(1'b0),
        .FIFO_RD_EN(1'b0),.RST(primitive_reset));
endmodule
`default_nettype wire
