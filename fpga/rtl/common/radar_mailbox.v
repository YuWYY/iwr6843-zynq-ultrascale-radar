`timescale 1ns/1ps
`default_nettype none
// Atomic bundled-data transfer, one outstanding request. Both ends MUST share
// the same asynchronous reset event, including when either clock is stopped.
// src_hold is held unchanged until the 3-FF acknowledge returns. The bundled
// path to dst_data is constrained by radar_cdc.xdc, not bitwise synchronized.
module radar_mailbox #(
    parameter integer WIDTH=32,
    parameter integer EXTERNAL_SRC_RESET=0,
    parameter integer EXTERNAL_DST_RESET=0
)(
    input wire src_clk, input wire dst_clk, input wire arstn,
    input wire src_resetn_in, input wire dst_resetn_in,
    input wire src_send, input wire [WIDTH-1:0] src_data,
    output wire src_busy, output reg src_done,
    output reg dst_valid, output reg [WIDTH-1:0] dst_data
);
    wire src_resetn, dst_resetn;
    // External inputs must be the shared async-assert/sync-release owner for
    // this epoch and local clock. Never feed a raw asynchronous reset here.
    generate
        if (EXTERNAL_SRC_RESET) begin : g_src_external
            assign src_resetn = src_resetn_in;
        end else begin : g_src_reset
            radar_reset_release #(.HOLD_CYCLES(1)) u_src_release(
                .clk(src_clk),.allow_n(arstn),.resetn(src_resetn));
        end
        if (EXTERNAL_DST_RESET) begin : g_dst_external
            assign dst_resetn = dst_resetn_in;
        end else begin : g_dst_reset
            radar_reset_release #(.HOLD_CYCLES(1)) u_dst_release(
                .clk(dst_clk),.allow_n(arstn),.resetn(dst_resetn));
        end
    endgenerate
    (* KEEP = "TRUE" *) reg [WIDTH-1:0] src_hold;
    reg req_toggle, ack_toggle, ack_seen;
    (* ASYNC_REG="TRUE", SHREG_EXTRACT="NO" *) reg [2:0] req_ff, ack_ff;
    assign src_busy = !src_resetn || (req_toggle != ack_ff[2]);
    always @(posedge src_clk or negedge src_resetn) begin
        if (!src_resetn) begin
            src_hold<=0; req_toggle<=0; ack_ff<=0; ack_seen<=0; src_done<=0;
        end else begin
            ack_ff <= {ack_ff[1:0], ack_toggle};
            src_done <= (ack_seen != ack_ff[2]);
            ack_seen <= ack_ff[2];
            if (src_send && !src_busy) begin
                src_hold <= src_data; req_toggle <= ~req_toggle;
            end
        end
    end
    always @(posedge dst_clk or negedge dst_resetn) begin
        if (!dst_resetn) begin req_ff<=0; ack_toggle<=0; dst_valid<=0; dst_data<=0; end
        else begin
            req_ff <= {req_ff[1:0], req_toggle};
            dst_valid <= 0;
            if (req_ff[2] != ack_toggle) begin
                dst_data <= src_hold;
                dst_valid <= 1;
                ack_toggle <= req_ff[2];
            end
        end
    end
endmodule
`default_nettype wire
