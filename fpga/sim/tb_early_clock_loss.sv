`timescale 1ns/1ps
// Control-domain regression: a real RX first-data notification may precede
// the first divided heartbeat. Integration TB separately drives the PHY/FIFO.
module tb_early_clock_loss;
    reg clk=0; always #5 clk=~clk;
    reg rn=0, first=0, hb=0, seq=0;
    reg [31:0] ctrl=0;
    wire [31:0] response; wire [15:0] faults;
    wire opened, abort_level, recovery; wire [3:0] io;
    radar_control #(.RX_CLOCK_TIMEOUT(200),.FIRST_DATA_TIMEOUT(1000)) dut(
        .clk(clk),.resetn(rn),.ctrl_i(ctrl),.status_o(response),.io_cmd(io),
        .pg(1'b1),.pg_qualified(1'b1),.nerr_n(1'b1),.power_lost(1'b0),
        .io_active(1'b1),.tx_oe(1'b1),.nrst_low(1'b0),.sync_oe(1'b1),
        .arm_admissible_sys(1'b1),.ref_cal_ready_sys(1'b1),.rx_ready_sys(1'b1),
        .baseline_valid_sys(1'b1),.baseline_data(27'd0),.baseline_generation_sys(32'd0),
        .cfg_result(4'd0),.cfg_phase(3'd0),.phy_fault_sys(4'd0),.cfg_generation(32'd0),
        .deser_ready(1'b0),.fifo_ready(1'b0),.dma_stream_resetn(1'b1),
        .rx_heartbeat(hb),.rx_complete(1'b0),.rx_fault(1'b0),.rx_first(first),.rx_pending(1'b0),.rx_idle(1'b0),
        .cmd_busy(1'b0),.cmd_send(),.cmd_data(),.snapshot_busy(1'b0),.snapshot_valid(1'b0),
        .snapshot_data(384'd0),.snapshot_generation(32'd0),.snapshot_request(),
        .tap_snapshot_valid(1'b0),.tap_snapshot_data(64'd0),.tap_snapshot_generation(32'd0),
        .delay_busy(1'b0),.delay_send(),.delay_data(),.recover_active(recovery),
        .abort_level(abort_level),.session_open(opened),.sys_faults(faults));
    task command(input [6:0] op,input [7:0] idx,input [15:0] arg);
        @(negedge clk);seq=~seq;ctrl={seq,op,idx,arg};@(posedge clk);#0.1;
        if(response[30]) $fatal(1,"REGRESSION_ASSERT command rejected %h",response);
    endtask
    initial begin
        repeat(5) @(negedge clk);rn=1;
        command(2,4,0);command(3,4,16'h1616);command(5,0,0);
        repeat(20) @(negedge clk);
        if(faults) $fatal(1,"REGRESSION_ASSERT fault before first activity");
        first=1; // First pair reached SYS; RX stops before heartbeat bit7 toggles.
        repeat(210) @(negedge clk);
        if(!faults[3] || !abort_level || !opened)
            $fatal(1,"REGRESSION_ASSERT early first-data then clock loss not latched faults=%h",faults);
        command(8,0,16'hc1ea);first=0;repeat(140) @(negedge clk);
        if(faults || opened) $fatal(1,"REGRESSION_ASSERT recovery did not clear session");
        command(5,0,0);repeat(20) @(negedge clk);first=1;
        repeat(5) begin repeat(100) @(negedge clk);hb=~hb;end
        if(faults) $fatal(1,"REGRESSION_ASSERT healthy heartbeat falsely faulted");
        $display("PASS tb_early_clock_loss: pre-heartbeat first-data timeout, sticky fault, recovery, healthy activity");$finish;
    end
    initial begin #100000;$fatal(1,"REGRESSION_ASSERT watchdog");end
endmodule
