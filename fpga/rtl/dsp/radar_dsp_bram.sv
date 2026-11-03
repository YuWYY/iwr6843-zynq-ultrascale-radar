`timescale 1ns/1ps
// Explicit AMD block-memory primitive contract. No asynchronous read/reset,
// no automatic fallback to LUT memory. Callers initialize every valid address.
module radar_dsp_bram #(
    parameter integer WIDTH = 64,
    parameter integer DEPTH = 8192,
    parameter integer ADDR_WIDTH = 13
) (
    input wire clk,
    input wire we,
    input wire [ADDR_WIDTH-1:0] waddr,
    input wire [WIDTH-1:0] wdata,
    input wire [ADDR_WIDTH-1:0] raddr,
    output wire [WIDTH-1:0] rdata
);
    xpm_memory_sdpram #(
        .ADDR_WIDTH_A(ADDR_WIDTH), .ADDR_WIDTH_B(ADDR_WIDTH),
        .AUTO_SLEEP_TIME(0), .BYTE_WRITE_WIDTH_A(WIDTH),
        .CASCADE_HEIGHT(0), .CLOCKING_MODE("common_clock"),
        .ECC_MODE("no_ecc"), .MEMORY_INIT_FILE("none"),
        .MEMORY_INIT_PARAM("0"), .MEMORY_OPTIMIZATION("true"),
        .MEMORY_PRIMITIVE("block"), .MEMORY_SIZE(WIDTH*DEPTH),
        .MESSAGE_CONTROL(0), .READ_DATA_WIDTH_B(WIDTH),
        .READ_LATENCY_B(1), .READ_RESET_VALUE_B("0"),
        .RST_MODE_A("SYNC"), .RST_MODE_B("SYNC"), .SIM_ASSERT_CHK(1),
        .USE_EMBEDDED_CONSTRAINT(0), .USE_MEM_INIT(0),
        .WAKEUP_TIME("disable_sleep"), .WRITE_DATA_WIDTH_A(WIDTH),
        .WRITE_MODE_B("read_first")
    ) storage (
        .clka(clk), .clkb(clk), .ena(1'b1), .enb(1'b1),
        .wea(we), .addra(waddr), .dina(wdata), .addrb(raddr),
        .doutb(rdata), .regceb(1'b1), .rstb(1'b0), .sleep(1'b0),
        .injectdbiterra(1'b0), .injectsbiterra(1'b0),
        .dbiterrb(), .sbiterrb()
    );
endmodule
