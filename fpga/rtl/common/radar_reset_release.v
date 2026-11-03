`timescale 1ns/1ps
`default_nettype none
// Raw asynchronous reset reaches only the vendor 3-FF reset synchronizer.
// The hold counter uses its destination-synchronous qualification, never the
// raw reset. The output can still assert with the destination clock stopped.
// Deassertion is after 3 synchronizer edges plus HOLD_CYCLES local edges.
module radar_reset_release #(
    parameter integer HOLD_CYCLES = 64,
    parameter integer PREQUALIFIED = 0
)(
    input wire clk, input wire allow_n, output wire resetn
);
    localparam integer COUNT_WIDTH=(HOLD_CYCLES<=1) ? 1 : $clog2(HOLD_CYCLES);
    wire permit_n;
    generate
        if (PREQUALIFIED) begin : g_local
            // The caller owns ONE release for this clock domain. Do not
            // resynchronize it: all reconvergent consumers share that edge.
            assign permit_n = allow_n;
        end else begin : g_async
            xpm_cdc_async_rst #(.DEST_SYNC_FF(3), .INIT_SYNC_FF(0), .RST_ACTIVE_HIGH(0))
                u_reset_sync(.src_arst(allow_n), .dest_clk(clk), .dest_arst(permit_n));
        end
    endgenerate
    reg [COUNT_WIDTH-1:0] count;
    reg released;
    always @(posedge clk) begin
        if (!permit_n) count<=0;
        else if (!released && count < HOLD_CYCLES-1) count<=count+1'b1;
    end
    // permit_n asserts asynchronously but deasserts only on this clk. Keep
    // this local recovery/removal path timed; it is NOT a raw-CLR exception.
    always @(posedge clk or negedge permit_n) begin
        if (!permit_n) released<=0;
        else if (count == HOLD_CYCLES-1) released<=1;
    end
    assign resetn = released;
endmodule
`default_nettype wire
