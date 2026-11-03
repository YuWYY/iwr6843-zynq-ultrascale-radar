`timescale 1ns/1ps
module tb_control;
    reg clk=0;always #5 clk=~clk;
    reg rn=0;reg [31:0] ctrl=0;wire [31:0] response;wire [3:0] io;
    reg pg=1,pq=1,ne=1,pl=0,ia=0,tx=0,nrst=0,syn=0,phy=1,des=0,fifo=0,dmarn=1,hb=0;
    reg arm_ok=1,rx_ready=1,baseline_valid=1;
    reg [3:0] phy_fault=0;
    reg done=0,fault=0,first=0,pending=0,idle=0,cb=0;
    wire cs;wire [143:0] cd;
    reg sb=0,sv=0;reg [383:0] sd=0;reg [31:0] sg=0;wire sr;
    reg tv=0;reg [63:0] td=0;reg [31:0] tg=0;reg db=0;wire ds;wire [27:0] dd;
    wire recovery,abort_level,opened,transaction_cancel;wire [15:0] faults;
    reg seq=0;
    radar_control #(.RX_CLOCK_TIMEOUT(100),.FIRST_DATA_TIMEOUT(1000)) dut(
        .clk(clk),.resetn(rn),.ctrl_i(ctrl),.status_o(response),.io_cmd(io),
        .pg(pg),.pg_qualified(pq),.nerr_n(ne),.power_lost(pl),.io_active(ia),
        .tx_oe(tx),.nrst_low(nrst),.sync_oe(syn),
        .arm_admissible_sys(arm_ok),.ref_cal_ready_sys(phy),.rx_ready_sys(rx_ready),
        .baseline_valid_sys(baseline_valid),.baseline_data(27'h1234567),.baseline_generation_sys(32'd9),
        .cfg_result(4'd5),.cfg_phase(3'd5),.phy_fault_sys(phy_fault),.cfg_generation(32'd12),
        .deser_ready(des),.fifo_ready(fifo),
        .dma_stream_resetn(dmarn),.rx_heartbeat(hb),.rx_complete(done),.rx_fault(fault),
        .rx_first(first),.rx_pending(pending),.rx_idle(idle),.cmd_busy(cb),.cmd_send(cs),.cmd_data(cd),
        .snapshot_busy(sb),.snapshot_valid(sv),.snapshot_data(sd),.snapshot_generation(sg),.snapshot_request(sr),
        .tap_snapshot_valid(tv),.tap_snapshot_data(td),.tap_snapshot_generation(tg),
        .delay_busy(db),.delay_send(ds),.delay_data(dd),
        .recover_active(recovery),.abort_level(abort_level),.transaction_cancel(transaction_cancel),
        .session_open(opened),.sys_faults(faults));
    task command(input reg [6:0] op,input reg [7:0] index,input reg [15:0] arg,input integer expect_code);
        @(negedge clk);seq=~seq;ctrl={seq,op,index,arg};
        @(posedge clk);#0.1;
        if(response[31]!==seq || response[29:24]!==6'(expect_code) || response[30]!=(expect_code!=0))
            $fatal(1,"CSR opcode=%h response=%h expected code=%0d",op,response,expect_code);
    endtask
    initial begin
        repeat(5) @(negedge clk);rn=1;
        command(4,0,0,0);if(response[15:0]!=16'h0101) $fatal(1,"ABI low");
        command(4,0,1,0);if(response[15:0]!=16'h5243) $fatal(1,"ABI high");
        command(5,0,0,6); // Explicit RAW16 confirmation missing.
        command(2,4,16'h0008,0);command(3,4,16'h1616,0);
        command(5,0,0,5); // Digital IO not enabled.
        command(1,0,3,0);if(io!=3) $fatal(1,"IO_SET");ia=1;tx=1;
        command(5,0,0,0);
        if(!opened || !cs || cd[143:136]!=1 || cd[31:0]!=32768) $fatal(1,"ARM not atomic queued");
        command(2,0,16'h1234,2); // immutable active configuration
        command(5,0,0,7);
        command(6,0,0,0);if(!cs || cd[143:136]!=2) $fatal(1,"STOP queue");
        command(7,0,0,0);if(!faults[8]) $fatal(1,"abort not sticky");
        command(8,0,16'h0000,8);
        command(8,0,16'hc1ea,0);if(opened||faults||!recovery) $fatal(1,"recover protocol");
        command(5,0,0,2);repeat(140) @(negedge clk);
        command(4,8'h20,0,5); // snapshot cannot masquerade as zero data.
        command(9,0,0,0);if(!sr) $fatal(1,"snapshot request");
        sb=1;command(9,0,0,2);sb=0;sv=1;sg=1;sd[31:0]=32'hdeadbeef;
        command(4,8'h20,0,0);if(response[15:0]!=16'hbeef) $fatal(1,"snapshot low");
        command(4,8'h20,1,0);if(response[15:0]!=16'hdead) $fatal(1,"snapshot high");
        command(2,5,16'd42,0);command(10,0,0,0);if(!ds || dd[8:0]!=42) $fatal(1,"tap apply");
        command(9,0,0,2);if(sr) $fatal(1,"SNAPSHOT raced DELAY send");
        db=1;command(9,0,0,2);if(sr) $fatal(1,"SNAPSHOT accepted during DELAY execution");db=0;
        tv=1;td={32'd23,32'h01234567};tg=31;
        command(4,8'h0b,0,0);if(response[15:0]!=16'h4567) $fatal(1,"measured tap value page");
        command(4,8'h0c,0,0);if(response[15:0]!=23) $fatal(1,"measurement generation mistaken for snapshot copy");
        command(4,8'h13,0,0);if(response[15:0]!=31) $fatal(1,"copy generation diagnostics");
        command(9,0,0,0);command(10,0,0,2);if(ds) $fatal(1,"DELAY raced snapshot request");
        sb=1;command(10,0,0,2);command(11,0,0,2);sb=0;
        repeat(2) @(negedge clk);command(11,0,0,0);if(!ds || !dd[27]) $fatal(1,"recal");
        command(1,0,16'h0100,4);command(7'h7f,0,0,1);
        command(0,0,0,0);if(io!=0) $fatal(1,"SAFE");
        if(!transaction_cancel) $fatal(1,"SAFE without session did not cancel PHY");
        command(7,0,0,0);if(!transaction_cancel) $fatal(1,"ABORT without session did not cancel PHY");
        command(4,8'h10,0,0);if(response[15:0]!=12 || transaction_cancel) $fatal(1,"PHY result generation/cancel pulse");
        command(4,8'h12,0,0);if(response[15:0]!=9) $fatal(1,"baseline generation page");
        command(4,8'h11,0,0);if(response[15:0]!=16'h4567) $fatal(1,"baseline measured data page");
        command(4,8'h0f,0,0);if(response[15:0]!=16'h07d5) $fatal(1,"separate PHY status page");
        phy_fault=4;command(4,8'h14,0,0);if(response[15:0]!=4) $fatal(1,"independent automatic PHY fault page");
        command(4,8'h10,0,0);if(response[15:0]!=12) $fatal(1,"PHY fault changed software transaction generation");
        phy_fault=0;
        // REF can be ready before BITCLK. Only ARM may queue; measurements and
        // delay execution require the RX qualification and a real baseline.
        rx_ready=0;baseline_valid=0;
        command(10,0,0,5);command(9,0,0,5);command(4,8'h11,0,5);
        command(5,0,0,0);repeat(20) @(negedge clk);
        if(faults || abort_level || !opened) $fatal(1,"ARM before BITCLK falsely faulted");
        // Registered RX FAULT and COMPLETE arriving together cannot write a
        // successful end timestamp. The source-domain abort sets the same edge.
        @(negedge clk);done=1;fault=1;@(posedge clk);#0.1;
        if(!faults[7] || !abort_level) $fatal(1,"registered fault/abort latency");
        command(4,7,0,0);if(response[15:0]!=0) $fatal(1,"fault recorded a successful end timestamp");
        command(4,8,0,0);if(response[15:0]!=0) $fatal(1,"fault recorded high end timestamp");
        command(8,0,16'hc1ea,0);done=0;fault=0;rx_ready=1;baseline_valid=1;
        if(abort_level || faults) $fatal(1,"RECOVER did not clear abort atomically");
        repeat(140) @(negedge clk);
        command(5,0,0,0);@(negedge clk);first=1;repeat(8) @(negedge clk);done=1;
        repeat(3) @(negedge clk);command(4,7,0,0);
        if(response[15:0]==0 || faults) $fatal(1,"real completion missing end timestamp");
        // RECOVER and complete overlap: cancel/clear owns the edge.
        command(8,0,16'hc1ea,0);first=0;done=0;
        command(4,7,0,0);if(response[15:0]!=0) $fatal(1,"RECOVER retained old end timestamp");
        repeat(140) @(negedge clk);command(5,0,0,0);
        done=1;command(7,0,0,0);
        command(4,7,0,0);if(response[15:0]!=0 || !abort_level) $fatal(1,"ABORT same-cycle completion falsely succeeded");
        $display("PASS tb_control: ABI 1.0.1, pre-BITCLK ARM, ready-gated DELAY/snapshot, source-registered abort, fault/cancel wins completion, result/baseline pages");$finish;
    end
    initial begin #1000000;$fatal(1,"timeout");end
endmodule
