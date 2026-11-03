`timescale 1ns/1ps
`default_nettype none
// REF-only component reset/calibration supervisor. Sensor clocks may be absent.
// Stop the real clock buffers before changing primitive reset; release the
// IDELAY/ISERDES resets before IDELAYCTRL, as required by UG571 pp188-189.
module radar_phy_cal #(
    parameter integer RDY_TIMEOUT_CYCLES=300000
)(
    input wire clk_ref, input wire resetn_ref, input wire cal_enable,
    input wire gate_open_ref, input wire idelay_rdy,
    input wire recal_valid, input wire report_busy,
    output reg idelay_reset=1'b1, output reg primitive_reset=1'b1,
    output reg clock_enable_ref, output reg cal_ready, output reg executing,
    output reg report_send, output reg report_success
);
    localparam QUIESCE=0, HELD=1, RESET=2, PRIMITIVE_GUARD=3,
               WAIT_RDY=4, SETTLE=5, READY=6, FAILED=7;
    reg [2:0] state;
    reg [6:0] timer;
    reg [31:0] age;
    reg explicit_request, report_pending;
    // INIT=1 is the configuration/GSR exception. Runtime reset never asserts
    // these pins asynchronously: first the RX epoch restores EN_VTC high and
    // the clock buffers clear, then QUIESCE gives 8 REF cycles of guard.
    always @(posedge clk_ref) begin
        if(resetn_ref) begin
            if(state==QUIESCE && !gate_open_ref && timer==7) begin
                primitive_reset<=1; idelay_reset<=1;
            end
            if(cal_enable && !recal_valid) begin
                if(state==RESET && timer==63) primitive_reset<=0;
                if(state==PRIMITIVE_GUARD && timer==15) idelay_reset<=0;
            end
        end
    end
    always @(posedge clk_ref or negedge resetn_ref) begin
        if(!resetn_ref) begin
            clock_enable_ref<=0; cal_ready<=0; executing<=0;
            report_send<=0; report_success<=0; state<=QUIESCE; timer<=0;
            age<=0; explicit_request<=0; report_pending<=0;
        end else begin
            report_send<=0;
            if(report_pending && !report_busy) begin
                report_pending<=0; report_send<=1;
            end
            case(state)
                QUIESCE: begin
                    clock_enable_ref<=0; cal_ready<=0;
                    if(gate_open_ref) timer<=0;
                    else if(timer==7) begin
                        timer<=0; state<=cal_enable ? RESET : HELD;
                    end else timer<=timer+1'b1;
                end
                HELD: if(cal_enable) begin timer<=0; state<=RESET; end
                RESET: begin
                    if(timer==63) begin timer<=0; state<=PRIMITIVE_GUARD; end
                    else timer<=timer+1'b1;
                end
                PRIMITIVE_GUARD: begin
                    // 16 REF clocks (53.3 ns) separate the primitive RST and
                    // controller RST edges. Native physical bound is < 8 ns.
                    if(timer==15) begin timer<=0; age<=0; state<=WAIT_RDY; end
                    else timer<=timer+1'b1;
                end
                WAIT_RDY: begin
                    if(idelay_rdy) begin timer<=0; state<=SETTLE; end
                    else if(age==RDY_TIMEOUT_CYCLES-1) begin
                        state<=FAILED; executing<=0;
                        // Boot failures are observable too; SYS records an
                        // independent fault without inventing a command result.
                        report_success<=0; report_pending<=1; explicit_request<=0;
                    end else age<=age+1'b1;
                end
                SETTLE: begin
                    if(!idelay_rdy) begin
                        state<=FAILED; executing<=0;
                        report_success<=0; report_pending<=1; explicit_request<=0;
                    end else if(timer==15) begin
                        clock_enable_ref<=1; cal_ready<=1; executing<=0; state<=READY;
                        if(explicit_request) begin
                            report_success<=1; report_pending<=1; explicit_request<=0;
                        end
                    end else timer<=timer+1'b1;
                end
                READY: if(!idelay_rdy) begin clock_enable_ref<=0; cal_ready<=0; state<=FAILED; end
                FAILED: begin clock_enable_ref<=0; cal_ready<=0; executing<=0; end
                default: begin clock_enable_ref<=0; cal_ready<=0; state<=QUIESCE; timer<=0; end
            endcase
            if(recal_valid && cal_enable) begin
                clock_enable_ref<=0; cal_ready<=0; executing<=1;
                timer<=0; age<=0; explicit_request<=1; state<=QUIESCE;
            end
            // SYS isolation resets both mailboxes immediately. The REF owner
            // stays alive to stop clocks and assert RST in order, even with
            // BITCLK stopped. A cancelled report cannot survive this level.
            if(!cal_enable) begin
                clock_enable_ref<=0; cal_ready<=0; executing<=0;
                report_send<=0; report_pending<=0; explicit_request<=0;
                if(state!=QUIESCE && state!=HELD) begin state<=QUIESCE; timer<=0; end
            end
        end
    end
endmodule
`default_nettype wire
