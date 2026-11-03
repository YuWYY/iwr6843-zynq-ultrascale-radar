`timescale 1ns/1ps
`default_nettype none
// sys requests a coherent RX snapshot. Frozen result remains valid until the
// next request completes. No clock -> busy, NOT an invented all-zero snapshot.
module radar_snapshot #(
    parameter integer WIDTH=256,
    parameter integer EXTERNAL_SYS_RESET=0,
    parameter integer EXTERNAL_RX_RESET=0
)(
    input wire sys_clk, input wire rx_clk, input wire arstn,
    input wire sys_resetn_in, input wire rx_resetn_in,
    input wire request, input wire sample_valid, input wire [WIDTH-1:0] rx_data,
    output wire busy, output reg valid, output reg [WIDTH-1:0] data,
    output reg [31:0] generation
);
    wire sys_resetn, rx_resetn;
    // A shared per-domain owner avoids independently released resets feeding
    // reconvergent logic. Defaults preserve standalone legacy test instances.
    generate
        if (EXTERNAL_SYS_RESET) begin : g_sys_external
            assign sys_resetn = sys_resetn_in;
        end else begin : g_sys_reset
            radar_reset_release #(.HOLD_CYCLES(1)) u_sys_release(
                .clk(sys_clk),.allow_n(arstn),.resetn(sys_resetn));
        end
        if (EXTERNAL_RX_RESET) begin : g_rx_external
            assign rx_resetn = rx_resetn_in;
        end else begin : g_rx_reset
            radar_reset_release #(.HOLD_CYCLES(1)) u_rx_release(
                .clk(rx_clk),.allow_n(arstn),.resetn(rx_resetn));
        end
    endgenerate
    reg req_toggle, ack_toggle;
    (* KEEP="TRUE" *) reg [WIDTH-1:0] rx_hold;
    (* ASYNC_REG="TRUE", SHREG_EXTRACT="NO" *) reg [2:0] req_ff, ack_ff;
    reg ack_seen;
    assign busy = !sys_resetn || (req_toggle != ack_seen);
    always @(posedge sys_clk or negedge sys_resetn) begin
        if (!sys_resetn) begin req_toggle<=0; ack_ff<=0; ack_seen<=0; valid<=0; data<=0; generation<=0; end
        else begin
            ack_ff <= {ack_ff[1:0], ack_toggle};
            if (request && !busy) req_toggle <= ~req_toggle;
            if (ack_seen != ack_ff[2]) begin
                data <= rx_hold; valid<=1; generation<=generation+1'b1;
                ack_seen <= ack_ff[2];
            end
        end
    end
    always @(posedge rx_clk or negedge rx_resetn) begin
        if (!rx_resetn) begin req_ff<=0; ack_toggle<=0; rx_hold<=0; end
        else begin
            req_ff <= {req_ff[1:0], req_toggle};
            // A stale/invalid measurement must not manufacture a new generation.
            // Keep the request outstanding until a valid sample or common cancel.
            if (req_ff[2] != ack_toggle && sample_valid) begin
                rx_hold <= rx_data; ack_toggle <= req_ff[2];
            end
        end
    end
endmodule
`default_nettype wire
