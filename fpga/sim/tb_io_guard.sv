`timescale 1ns/1ps
module tb_io_guard;
    reg clk=0;always #5 clk=~clk;
    reg rn=0,pg=0,ne=1,rx=1,tx=1;reg [3:0] cmd=0;
    wire rxo,txo,oe,nr,sd,se,pgs,ns,pq,pl,active;
    radar_io_guard #(.POWER_QUAL_CYCLES(4)) dut(.clk(clk),.resetn(rn),.io_cmd(cmd),
        .pg_raw(pg),.nerr_n_raw(ne),.uart_rx_raw(rx),.ps_uart_tx(tx),.ps_uart_rx(rxo),
        .tx_data(txo),.tx_oe(oe),.nrst_low(nr),.sync_data(sd),.sync_oe(se),
        .pg_sync(pgs),.nerr_n_sync(ns),.power_qualified(pq),.power_lost(pl),.io_active(active));
    task waitcycles(input integer n);repeat(n) @(negedge clk);endtask
    initial begin
        waitcycles(4);rn=1;waitcycles(4);
        if(oe||nr||se||!rxo) $fatal(1,"default not safe");
        cmd=3;waitcycles(8);if(oe) $fatal(1,"driving unpowered radar");
        pg=1;waitcycles(16);if(!pq||!oe||nr) $fatal(1,"qualified enable");
        rx=0;waitcycles(6);if(rxo) $fatal(1,"UART RX gated incorrectly");
        cmd=7;waitcycles(2);if(oe||!nr||se||!rxo) $fatal(1,"reset safety");
        cmd=3;waitcycles(2);pg=0;#1;
        if(oe||nr||se) $fatal(1,"raw power loss did not immediately mask outputs");
        waitcycles(8);if(!pl) $fatal(1,"power loss not sticky");
        pg=1;waitcycles(16);if(oe) $fatal(1,"outputs re-enabled without acknowledgment");
        cmd=0;waitcycles(3);cmd=3;waitcycles(3);if(!oe) $fatal(1,"explicit re-enable failed");
        ne=0;waitcycles(6);if(ns) $fatal(1,"NERR sampling");
        rn=0;#1;if(oe||nr||se||!rxo) $fatal(1,"reset safety immediate");
        $display("PASS tb_io_guard: default/power-qual/UART/NRST/dropout/re-arm/reset");$finish;
    end
    initial begin #100000;$fatal(1,"timeout");end
endmodule
