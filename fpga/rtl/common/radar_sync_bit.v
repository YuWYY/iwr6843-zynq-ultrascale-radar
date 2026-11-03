`timescale 1ns/1ps
`default_nettype none
// Single-bit level CDC only. Never use this module for a multi-bit data bus.
module radar_sync_bit #(parameter INIT = 1'b0)(
    input wire clk, input wire arstn, input wire din, output wire dout
);
    (* ASYNC_REG = "TRUE", SHREG_EXTRACT = "NO" *) reg [2:0] sync_ff;
    always @(posedge clk or negedge arstn)
        if (!arstn) sync_ff <= {3{INIT}};
        else sync_ff <= {sync_ff[1:0], din};
    assign dout = sync_ff[2];
endmodule
`default_nettype wire
