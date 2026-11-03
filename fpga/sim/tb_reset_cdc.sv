`timescale 1ns/1ps
// T5: real XPM reset chain, stopped-high/low clock, and common cancellation of
// both bundled-data ends. All local consumers share one release owner;
// the longer FIFO-like HOLD is synchronous to that qualification.
// No forced internal register or fabricated ACK.
module tb_reset_cdc;
    reg sc=0,rc=0,run_rx=1,rn=0;
    always #5 sc=~sc;
    always #6.7 if(run_rx) rc=~rc;
    wire released, shared_sys, shared_rx;
    radar_reset_release #(.HOLD_CYCLES(1)) sys_owner(
        .clk(sc),.allow_n(rn),.resetn(shared_sys));
    radar_reset_release #(.HOLD_CYCLES(1)) rx_owner(
        .clk(rc),.allow_n(rn),.resetn(shared_rx));
    radar_reset_release #(.HOLD_CYCLES(8),.PREQUALIFIED(1)) rr(
        .clk(rc),.allow_n(shared_rx),.resetn(released));
    reg send=0,request=0,sample_valid=1;
    reg [31:0] payload=0,expected=0;
    wire busy,done,valid,sbusy,svalid;
    wire [31:0] received,sdata,generation;
    integer received_count=0,release_count=0,checks=0,qualified_cycle=0;
    radar_mailbox #(.WIDTH(32),.EXTERNAL_SRC_RESET(1),.EXTERNAL_DST_RESET(1)) mb(
        .src_clk(sc),.dst_clk(rc),.arstn(rn),
        .src_resetn_in(shared_sys),.dst_resetn_in(shared_rx),
        .src_send(send),.src_data(payload),.src_busy(busy),.src_done(done),.dst_valid(valid),.dst_data(received));
    radar_snapshot #(.WIDTH(32),.EXTERNAL_SYS_RESET(1),.EXTERNAL_RX_RESET(1)) ss(
        .sys_clk(sc),.rx_clk(rc),.arstn(rn),
        .sys_resetn_in(shared_sys),.rx_resetn_in(shared_rx),
        .request(request),.sample_valid(sample_valid),.rx_data(32'h98765432),
        .busy(sbusy),.valid(svalid),.data(sdata),.generation(generation));
    always @(negedge rn) release_count=0;
    always @(posedge rc) if(rn) release_count=release_count+1;
    always @(posedge shared_rx) qualified_cycle=release_count;
    always @(posedge released) begin
        if(release_count<12 || release_count-qualified_cycle<8)
            $fatal(1,"REGRESSION_ASSERT early qualified HOLD release: %0d qualification=%0d",release_count,qualified_cycle);
        checks=checks+1;
    end
    always @(posedge sc or posedge rc) begin
        #0.02;
        if(mb.src_resetn!==shared_sys || ss.sys_resetn!==shared_sys ||
           mb.dst_resetn!==shared_rx || ss.rx_resetn!==shared_rx || rr.permit_n!==shared_rx)
            $fatal(1,"REGRESSION_ASSERT local consumers did not share exact release");
    end
    always @(posedge rc) begin
        #0.02;
        if(valid) begin
            if(received!==expected) $fatal(1,"REGRESSION_ASSERT stale/torn mailbox %h expected %h",received,expected);
            received_count=received_count+1;
        end
    end
    task sendword(input [31:0] word);
        wait(!busy);@(negedge sc);payload=word;expected=word;send=1;
        @(negedge sc);send=0;wait(!busy);repeat(4) @(negedge sc);
    endtask
    task snaprequest;
        wait(!sbusy);@(negedge sc);request=1;@(negedge sc);request=0;
    endtask
    initial begin
        // Explicit pulse also tests asynchronously asserting before any RX edge.
        #0.2;rn=1;#0.2;rn=0;
        repeat(6) @(negedge sc);rn=1;wait(released);
        sendword(32'h10000000);
        if(received_count!=1) $fatal(1,"REGRESSION_ASSERT initial transaction");
        sample_valid=0;snaprequest();repeat(30) @(negedge sc);
        if(!sbusy || generation!=0 || svalid) $fatal(1,"REGRESSION_ASSERT invalid sample acknowledged");
        sample_valid=1;wait(!sbusy);repeat(3) @(negedge sc);
        if(generation!=1 || !svalid || sdata!==32'h98765432) $fatal(1,"REGRESSION_ASSERT valid snapshot missing");
        for(integer n=0;n<8;n=n+1) begin
            if(n[0]) @(posedge rc); else @(negedge rc);
            #0.1;run_rx=0;
            @(negedge sc);payload=32'hbad00000+n;send=1;
            @(negedge sc);send=0;
            snaprequest();repeat(12) @(negedge sc);
            if(!busy || !sbusy || received_count!=n+1)
                $fatal(1,"REGRESSION_ASSERT stopped clock fabricated completion");
            #(0.3+n*0.13);rn=0;#0.02;
            if(released!==0 || shared_rx!==0 || mb.dst_resetn!==0 || ss.rx_resetn!==0)
                $fatal(1,"REGRESSION_ASSERT stopped clock reset did not assert to all consumers");
            // Include a one-SYS-period reset pulse wholly between RX edges.
            // Async assertion must be remembered after raw rn returns high.
            if(n[0]) #10; else repeat(4) @(negedge sc);
            #(0.1+n*0.31);rn=1;
            repeat(12) @(negedge sc);
            if(released!==0 || shared_rx!==0 || mb.dst_resetn!==0 || ss.rx_resetn!==0 || generation!=0 || svalid)
                $fatal(1,"REGRESSION_ASSERT reset released without RX or stale snapshot");
            run_rx=1;wait(released);repeat(12) @(negedge sc);
            if(received_count!=n+1 || busy || sbusy || generation!=0)
                $fatal(1,"REGRESSION_ASSERT old req/ack replayed after reset");
            sendword(32'h20000000+n);
            if(received_count!=n+2) $fatal(1,"REGRESSION_ASSERT post-cancel mailbox locked");
        end
        if(checks!=9) $fatal(1,"REGRESSION_ASSERT missed release cases %0d",checks);
        $display("PASS tb_reset_cdc: one shared XPM release per domain, exact mailbox/snapshot release identity, qualified HOLD, short reset pulses with RX stopped high/low, valid-only snapshots, no old req/ACK replay");
        $finish;
    end
    initial begin #100000;$fatal(1,"REGRESSION_ASSERT SYS deadline");end
endmodule
