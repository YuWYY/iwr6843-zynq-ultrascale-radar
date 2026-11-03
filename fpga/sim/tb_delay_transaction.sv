`timescale 1ns/1ps
// Real GPIO control + production PHY, actual AMD primitives and CDC channels.
module tb_delay_transaction;
    reg sys=0,refclk=0,bclk=0,rn=0,rr=1,seq=0;
    real phase_ns=0.75;
    always #5 sys=~sys;
    initial begin
        if($test$plusargs("PHASE000")) phase_ns=0.0;
        if($test$plusargs("PHASE025")) phase_ns=0.25;
        if($test$plusargs("PHASE125")) phase_ns=1.25;
        if($test$plusargs("PHASE225")) phase_ns=2.25;
        #(phase_ns);forever #1.666667 refclk=~refclk;
    end
    always #1.666667 bclk=~bclk;
    reg [31:0] ctrl=0;wire [31:0] response;
    wire send,busy,ready,refready,arm,bvalid,cancel,rx;
    wire [27:0] config_data;wire [26:0] readback,baseline;
    wire [3:0] result;wire [2:0] phase;wire [31:0] generation,bgen;
    integer accepted=0,loads=0,code=0;
    radar_lvds_phy phy(.sys_clk(sys),.sys_resetn(rn),.ref_clk(refclk),.ref_reset(rr),
        .cfg_send(send),.cfg_data(config_data),.cancel_sys(cancel),.cfg_busy(busy),.ready_sys(ready),
        .arm_admissible_sys(arm),.ref_cal_ready_sys(refready),.rx_ready_sys(),
        .baseline_valid_sys(bvalid),.baseline_data(baseline),.baseline_generation_sys(bgen),
        .cfg_result(result),.cfg_phase(phase),.cfg_generation(generation),
        .bitclk_p(bclk),.bitclk_n(~bclk),.frclk_p(1'b0),.frclk_n(1'b1),
        .data0_p(1'b0),.data0_n(1'b1),.data1_p(1'b0),.data1_n(1'b1),
        .rx_clk(rx),.deser_resetn(),.fr_byte(),.data0_byte(),.data1_byte(),
        .delay_readback(readback),.ref_ready());
    radar_control control(.clk(sys),.resetn(rn),.ctrl_i(ctrl),.status_o(response),.io_cmd(),
        .pg(1'b1),.pg_qualified(1'b1),.nerr_n(1'b1),.power_lost(1'b0),
        .io_active(1'b1),.tx_oe(1'b1),.nrst_low(1'b0),.sync_oe(1'b1),
        .arm_admissible_sys(arm),.ref_cal_ready_sys(refready),.rx_ready_sys(ready),
        .baseline_valid_sys(bvalid),.baseline_data(baseline),.baseline_generation_sys(bgen),
        .cfg_result(result),.cfg_phase(phase),.cfg_generation(generation),
        .deser_ready(1'b0),.fifo_ready(1'b0),.dma_stream_resetn(1'b1),
        .rx_heartbeat(1'b0),.rx_complete(1'b0),.rx_fault(1'b0),.rx_first(1'b0),.rx_pending(1'b0),.rx_idle(1'b0),
        .cmd_busy(1'b0),.cmd_send(),.cmd_data(),.snapshot_busy(1'b0),.snapshot_valid(1'b0),
        .snapshot_data(384'd0),.snapshot_generation(32'd0),.snapshot_request(),
        .tap_snapshot_valid(1'b0),.tap_snapshot_data(64'd0),.tap_snapshot_generation(32'd0),
        .delay_busy(busy),.delay_send(send),.delay_data(config_data),.recover_active(),
        .abort_level(),.transaction_cancel(cancel),.session_open(),.sys_faults());
    always @(posedge rx) if(rn && phy.delay_load!=0) loads=loads+1;
    task command(input [6:0] op,input [7:0] idx,input [15:0] arg);
        @(negedge sys);seq=~seq;ctrl={seq,op,idx,arg};@(posedge sys);#0.1;
        code=response[29:24];
        if(response[31]!==seq) $fatal(1,"REGRESSION_ASSERT command ACK mismatch");
        if(op==10 && code==0) accepted=accepted+1;
    endtask
    task idle_ready;
        integer age;begin
        // GPIO acceptance precedes the PHY's sampling of the registered send.
        // Let that hand-off happen before testing completion/ready.
        repeat(3) @(negedge sys);age=0;
        while((busy || !ready) && age<20000) begin @(negedge sys);age=age+1;end
        if(busy || !ready) $fatal(1,"REGRESSION_ASSERT PHY ready deadline");
        repeat(3) @(negedge sys);end
    endtask
    reg [31:0] previous_generation;
    initial begin
        #1000;rn=1;#100;rr=0;idle_ready();
        if(bgen!=1 || loads!=0 || readback!==baseline)
            $fatal(1,"REGRESSION_ASSERT calibration did not produce actual read-only baseline");
        command(2,5,42);
        repeat(160) begin command(10,0,0);if(code!=0 && code!=2 && code!=5) $fatal(1,"REGRESSION_ASSERT bad DELAY result");end
        idle_ready();
        $display("DELAY_PROBE phase=%f accepted=%0d terminal=%0d LOADrounds=%0d",phase_ns,accepted,generation,loads);
        if(generation!=accepted || readback!=={9'd0,9'd0,9'd42} || loads!=6)
            $fatal(1,"REGRESSION_ASSERT lost/duplicate transaction or non-stepped load");
        previous_generation=generation;command(10,0,0);if(code!=0) $fatal(1,"REGRESSION_ASSERT DELAY rejected");
        repeat(6) @(negedge sys);if(!busy) $fatal(1,"REGRESSION_ASSERT missing execution ownership");
        command(11,0,0);if(code!=2) $fatal(1,"REGRESSION_ASSERT RECAL overwritten active DELAY");
        idle_ready();if(generation!=previous_generation+1) $fatal(1,"REGRESSION_ASSERT duplicate terminal");
        previous_generation=generation;command(10,0,0);wait(phy.delay_state==1);force phy.idelay_rdy=0;
        repeat(40) @(negedge sys);
        if(busy || ready || result!=6 || generation!=previous_generation+1)
            $fatal(1,"REGRESSION_ASSERT RDY failure ownership/result");
        release phy.idelay_rdy;repeat(10) @(negedge sys);
        if(ready) $fatal(1,"REGRESSION_ASSERT spontaneous recovery");
        command(11,0,0);if(code!=0) $fatal(1,"REGRESSION_ASSERT failed PHY RECAL rejected");
        idle_ready();if(readback!==0 || bgen!=2) $fatal(1,"REGRESSION_ASSERT RECAL actual baseline");
        command(10,0,0);if(code!=0) $fatal(1,"REGRESSION_ASSERT post-RECAL DELAY rejected");idle_ready();
        if(readback!==42 || result!=1) $fatal(1,"REGRESSION_ASSERT post-RECAL result");
        $display("PASS tb_delay_transaction: GPIO acceptance/terminal identity, RX stepped real readback, execution BUSY, RDY failure, RECAL recovery");$finish;
    end
    initial begin #2000000;$fatal(1,"REGRESSION_ASSERT total SYS deadline");end
endmodule
