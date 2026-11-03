`timescale 1ns/1ps
module tb_cdc;
    reg sc=0,rc=0,run_rx=1,rn=0;
    always #5 sc=~sc;
    always #6.7 if(run_rx) rc=~rc; else rc=0;
    reg send=0;reg [31:0] payload=0;
    wire busy,done,v;wire [31:0] received;
    integer got=0;reg [31:0] expected=0;
    reg req=0;wire sb,sv;wire [63:0] snap;wire [31:0] gen;
    reg [31:0] counter=0;
    radar_mailbox #(.WIDTH(32)) dut(.src_clk(sc),.dst_clk(rc),.arstn(rn),
        .src_send(send),.src_data(payload),.src_busy(busy),.src_done(done),.dst_valid(v),.dst_data(received));
    always @(posedge rc or negedge rn) if(!rn) counter<=0;else counter<=counter+1;
    radar_snapshot #(.WIDTH(64)) ss(.sys_clk(sc),.rx_clk(rc),.arstn(rn),.request(req),.sample_valid(1'b1),
        .rx_data({~counter,counter}),.busy(sb),.valid(sv),.data(snap),.generation(gen));
    always @(posedge rc) begin #0.01;if(v) begin
        if(received!==expected) $fatal(1,"mailbox torn/wrong payload");got=got+1;end
    end
    task sendword(input reg [31:0] val);
        wait(!busy);@(negedge sc);payload=val;expected=val;send=1;
        @(negedge sc);send=0;payload=~val;wait(busy);wait(!busy);repeat(2) @(negedge sc);
    endtask
    initial begin
        repeat(5) @(negedge sc);rn=1;repeat(20) @(negedge sc);
        for(integer j=0;j<64;j=j+1) sendword(32'h5a001357+j*32'h3179);
        if(got!=64) $fatal(1,"mailbox duplicate/lost");
        for(integer j=0;j<8;j=j+1) begin
            wait(!sb);@(negedge sc);req=1;@(negedge sc);req=0;
            wait(sb);wait(!sb);repeat(2) @(negedge sc);
            if(!sv || snap[63:32]!==~snap[31:0] || gen!=j+1) $fatal(1,"snapshot not coherent");
        end
        @(negedge rc);run_rx=0;repeat(5) @(negedge sc);
        @(negedge sc);payload=32'h12345678;expected=payload;send=1;
        @(negedge sc);send=0;repeat(30) @(negedge sc);
        if(!busy || got!=64) $fatal(1,"stopped destination fake completion");
        // Simultaneous common reset cancels the undelivered request.
        rn=0;repeat(10) @(negedge sc);rn=1;repeat(10) @(negedge sc);
        run_rx=1;repeat(30) @(negedge sc);
        if(got!=64) $fatal(1,"phantom transaction after reset");
        sendword(32'habcdef01);if(got!=65) $fatal(1,"post-reset transaction");
        $display("PASS tb_cdc: 65 transfers, coherent snapshots, stopped RX, reset cancellation");$finish;
    end
    initial begin #1000000;$fatal(1,"timeout");end
endmodule
