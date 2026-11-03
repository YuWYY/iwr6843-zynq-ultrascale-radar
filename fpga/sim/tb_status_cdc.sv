`timescale 1ns/1ps
// T4 source qualification with real byte decoder, packetizer and three-stage
// destination synchronizers. No force of state or replacement data path.
module tb_status_cdc;
    reg sc=0,rc=0,rn=0,srn=0,des=0;
    always #5 sc=~sc;
    initial begin #1.23;forever #6.667 rc=~rc;end
    reg cv=0,abort_level=0,ready=1;
    reg [143:0] cmd=0;
    reg [7:0] fr=0,d0=0,d1=0;
    wire [63:0] td;wire [7:0] tk;wire tv,tl,complete,fault,first,pending,idle;
    wire [383:0] snapshot;
    wire complete_sys,fault_sys,idle_sys;
    integer scenario=0,handshakes=0;
    radar_receive dut(.rx_clk(rc),.common_resetn(rn),.stream_resetn(srn),.deser_resetn(des),
        .cmd_valid(cv),.cmd_data(cmd),.abort_level(abort_level),.fr_byte(fr),.data0_byte(d0),.data1_byte(d1),
        .fifo_count(32'd0),.fifo_prog_full(1'b0),.fifo_almost_full(1'b0),
        .tdata(td),.tkeep(tk),.tvalid(tv),.tready(ready),.tlast(tl),.snapshot(snapshot),
        .complete(complete),.fault(fault),.first_seen(first),.pending(pending),.idle_observed(idle));
    radar_sync_bit cs(.clk(sc),.arstn(rn),.din(complete),.dout(complete_sys));
    radar_sync_bit fs(.clk(sc),.arstn(rn),.din(fault),.dout(fault_sys));
    radar_sync_bit ids(.clk(sc),.arstn(rn),.din(idle),.dout(idle_sys));
    always @(posedge sc) if(rn && scenario==1 && complete_sys)
        $fatal(1,"REGRESSION_ASSERT framing fault falsely became COMPLETE");
    always @(posedge rc) if(srn && tv && ready) begin
        handshakes=handshakes+1;
        if(scenario==2 && (td!==64'h4444333322221111 || tk!==8'hff || !tl))
            $fatal(1,"REGRESSION_ASSERT RAW16 payload changed %h",td);
    end
    task resetall;
        @(negedge sc);rn=0;srn=0;des=0;abort_level=0;fr=0;d0=0;d1=0;cv=0;ready=1;
        repeat(8) @(negedge rc);rn=1;srn=1;des=1;repeat(10) @(negedge rc);
        if(complete || fault || complete_sys || fault_sys) $fatal(1,"REGRESSION_ASSERT status reset");
    endtask
    task arm(input [31:0] pairs);
        @(negedge rc);cmd={8'h01,8'd0,32'd1,32'd0,32'd1,pairs};cv=1;
        @(negedge rc);cv=0;repeat(18) @(negedge rc);
        if(dut.state!=2 || !idle || !idle_sys) $fatal(1,"REGRESSION_ASSERT idle qualification/ARM");
    endtask
    task byte_in(input [7:0] f,input [7:0] a,input [7:0] b);
        @(negedge rc);fr=f;d0=a;d1=b;
    endtask
    initial begin
        resetall();scenario=1;arm(32'd8);
        byte_in(8'hff,8'h12,8'h56);byte_in(8'hff,8'h34,8'h78);
        repeat(10) byte_in(8'h00,0,0);
        repeat(10) @(negedge sc);
        if(!fault || !fault_sys || complete || dut.state!=5)
            $fatal(1,"REGRESSION_ASSERT ACTIVE to FAULT source status");
        repeat(16) @(negedge rc);
        if(!fault || !fault_sys) $fatal(1,"REGRESSION_ASSERT fault not persistent");
        resetall();scenario=2;arm(32'd2);ready=0;
        byte_in(8'hff,8'h11,8'h22);byte_in(8'h00,8'h11,8'h22);
        byte_in(8'hff,8'h33,8'h44);byte_in(8'h00,8'h33,8'h44);
        repeat(8) byte_in(0,0,0);
        if(!tv || !tl || complete || complete_sys || fault)
            $fatal(1,"REGRESSION_ASSERT stalled final beat falsely complete");
        repeat(12) @(negedge sc);
        if(complete_sys) $fatal(1,"REGRESSION_ASSERT completion before handshake");
        @(negedge rc);ready=1;repeat(10) @(negedge rc);
        if(!complete || !complete_sys || fault || handshakes!=1)
            $fatal(1,"REGRESSION_ASSERT real completion missing");
        // Idle is a level. Revoking deserializer qualification clears its RX
        // source register, then the 3FF SYS copy; it cannot remain sticky.
        des=0;repeat(6) @(negedge rc);
        if(idle || idle_sys) $fatal(1,"REGRESSION_ASSERT idle survived qualification loss");
        resetall();scenario=3;ready=0;
        @(negedge rc);cmd={8'h01,8'd0,32'd2,32'd0,32'd1,32'd2};cv=1;
        @(negedge rc);cv=0;repeat(20) @(negedge rc);
        if(!pending) $fatal(1,"REGRESSION_ASSERT ARM ignored FIFO readiness");
        @(negedge rc);cmd={8'h02,136'd0};cv=1;
        @(negedge rc);cv=0;repeat(8) @(negedge rc);
        if(pending || !complete_sys || tv || first || fault)
            $fatal(1,"REGRESSION_ASSERT canceled pending ARM terminal semantics");
        resetall();scenario=4;arm(32'd8);ready=0;
        byte_in(8'hff,8'h11,8'h22);byte_in(8'h00,8'h11,8'h22);
        byte_in(8'hff,8'h33,8'h44);byte_in(8'h00,8'h33,8'h44);
        repeat(8) byte_in(0,0,0);
        if(!tv) $fatal(1,"REGRESSION_ASSERT no held beat for cancellation");
        // A command-only cancellation resets control/decoder while stream data
        // remains valid until its actual handshake or stream reset.
        rn=0;repeat(6) @(negedge rc);
        if(!tv || tl || dut.state!=5 || td!==64'h4444333322221111)
            $fatal(1,"REGRESSION_ASSERT cancel discarded held AXIS or missed FAULT");
        $display("PASS tb_status_cdc: registered COMPLETE/FAULT/idle, ACTIVE-to-FAULT, final handshake, pending STOP, command-only cancel preserves AXIS");
        $finish;
    end
    initial begin #100000;$fatal(1,"REGRESSION_ASSERT SYS deadline");end
endmodule
