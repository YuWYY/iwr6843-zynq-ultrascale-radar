`timescale 1ns/1ps
`default_nettype none
// RAW16 physical-word decoder, NOT a TI HSI/chirp/frame parser.
// Byte bit[0] is earliest in time. Normalized FRCLK: idle=0; one word has
// 8 ones followed by 8 zeros. Sliding 24-bit windows recover ALL 8 bit phases
// without dropping a training prefix. First complete word can be delivered.
// cfg: [0] FR inversion, [1] D0 inversion, [2] D1 inversion,
//      [3] MSB-first serial data, [4] swap logical output lanes.
module radar_fclk16_decode(
    input wire clk, input wire resetn, input wire [4:0] cfg,
    input wire [7:0] fr_bits, input wire [7:0] d0_bits, input wire [7:0] d1_bits,
    output reg [31:0] pair_data, output reg pair_valid,
    output reg burst_first, output reg framing_error,
    output wire idle_gap, output reg [3:0] phase,
    output reg [31:0] words_seen, output reg [31:0] errors_seen
);
    reg [15:0] fh, h0, h1, mh;
    reg last_fr;
    reg [4:0] high_run, low_run;
    reg last_n;
    reg [4:0] high_n, low_n;
    reg [7:0] start_marks;
    reg bad;
    integer i, j, matches;
    reg [31:0] data_match;
    reg mark_match;
    reg [3:0] phase_match;
    wire [7:0] f = fr_bits ^ {8{cfg[0]}};
    wire [23:0] fw = {f, fh};
    wire [23:0] w0 = {d0_bits ^ {8{cfg[1]}}, h0};
    wire [23:0] w1 = {d1_bits ^ {8{cfg[2]}}, h1};
    wire [23:0] mw = {start_marks,mh};
    reg [15:0] a,b;
    function [15:0] rev16;
        input [15:0] v;
        integer k;
        begin for(k=0;k<16;k=k+1) rev16[k]=v[15-k]; end
    endfunction
    assign idle_gap = !last_fr && low_run >= 24;
    always @* begin
        last_n=last_fr; high_n=high_run; low_n=low_run;
        start_marks=0; bad=0;
        for(i=0;i<8;i=i+1) begin
            if (f[i]) begin
                if (!last_n) begin
                    start_marks[i]=(low_n >= 16);
                    if (low_n < 8) bad=1;
                    high_n=1;
                end else begin
                    if (high_n < 31) high_n=high_n+1'b1;
                    if (high_n > 8) bad=1;
                end
                low_n=0;
            end else begin
                if (last_n) begin
                    if (high_n != 8) bad=1;
                    low_n=1;
                end else if (low_n < 31) low_n=low_n+1'b1;
                high_n=0;
            end
            last_n=f[i];
        end
        matches=0; data_match=0; mark_match=0; phase_match=0; a=0; b=0;
        for(j=1;j<=8;j=j+1) begin
            if (fw[j +: 16] == 16'h00ff && !fw[j-1]) begin
                matches=matches+1;
                a=w0[j +: 16]; b=w1[j +: 16];
                if (cfg[3]) begin a=rev16(a); b=rev16(b); end
                data_match=cfg[4] ? {a,b} : {b,a};
                mark_match=mw[j]; phase_match=j;
            end
        end
    end
    always @(posedge clk or negedge resetn) begin
        if (!resetn) begin
            fh<=0; h0<=0; h1<=0; mh<=0; last_fr<=0; high_run<=0; low_run<=0;
            pair_data<=0; pair_valid<=0; burst_first<=0; framing_error<=0;
            phase<=0; words_seen<=0; errors_seen<=0;
        end else begin
            fh<={f,fh[15:8]}; h0<={d0_bits ^ {8{cfg[1]}},h0[15:8]};
            h1<={d1_bits ^ {8{cfg[2]}},h1[15:8]}; mh<={start_marks,mh[15:8]};
            last_fr<=last_n; high_run<=high_n; low_run<=low_n;
            pair_valid<=(matches==1)&&!bad;
            framing_error<=bad||(matches>1);
            burst_first<=0;
            if (matches==1 && !bad) begin
                pair_data<=data_match; burst_first<=mark_match; phase<=phase_match;
                words_seen<=words_seen+1'b1;
            end
            if (bad || matches>1) errors_seen<=errors_seen+1'b1;
        end
    end
endmodule
`default_nettype wire
