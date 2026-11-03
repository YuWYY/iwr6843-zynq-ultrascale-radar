`timescale 1ns/1ps
// End-to-end digital boundary regression. Real AMD UNISIM PHY and the CURRENT
// generated axis_data_fifo_0 model (8192x64, asynchronous, CDC=3) are required.
// The DMA-side sink/reset stimulus is a test endpoint, not a DMA/DDR model.
module tb_capture_fifo;
    localparam real UI=1.667;
    reg sys=0,refclk=0,bclk=0,bitclock_run=0;
    always #5 sys=~sys;
    initial begin #0.193;forever #UI refclk=~refclk;end
    initial begin #0.371;forever begin #UI;if(bitclock_run) bclk=~bclk;else bclk=0;end end
    reg rn=0,rr=1,dma_rn=1,fr=0,d0=0,d1=0;
    reg [31:0] ctrl=0;wire [31:0] status;
    reg seq=0;
    wire rx,cap_rn,sv,sr,sl,mv,ml;
    wire [63:0] sd,md;wire [7:0] sk,mk;
    wire [31:0] wr_count;wire prog_full,almost_full;
    reg mr=0,random_sink=0;
    reg [15:0] rng=16'h5a6d;
    integer stall_cycles=0;
    radar_capture_bd #(.POWER_QUAL_CYCLES(8),.RX_CLOCK_TIMEOUT(600),
        .FIRST_DATA_TIMEOUT(200000)) dut(
        .clk_sys(sys),.rstn_sys(rn),.clk_ref(refclk),.rst_ref(rr),.dma_stream_resetn(dma_rn),
        .ctrl_i(ctrl),.status_o(status),.ps_uart_tx_i(1'b1),.ps_uart_rx_o(),
        .radar_uart_rx_raw(1'b1),.radar_3v3_raw(1'b1),.radar_nerr_n_raw(1'b1),
        .radar_uart_tx_data(),.radar_uart_tx_oe(),.radar_nrst_low(),.radar_sync_data(),.radar_sync_oe(),
        .bitclk_p(bclk),.bitclk_n(~bclk),.frclk_p(fr),.frclk_n(~fr),
        .data0_p(d0),.data0_n(~d0),.data1_p(d1),.data1_n(~d1),
        .rx_clk_o(rx),.capture_resetn(cap_rn),.m_axis_tdata(sd),.m_axis_tkeep(sk),
        .m_axis_tvalid(sv),.m_axis_tready(sr),.m_axis_tlast(sl),
        .fifo_wr_count(wr_count),.fifo_prog_full(prog_full),.fifo_almost_full(almost_full));
    radar_system_axis_data_fifo_0_0 fifo(
        .s_axis_aclk(rx),.m_axis_aclk(sys),.s_axis_aresetn(cap_rn),
        .s_axis_tdata(sd),.s_axis_tkeep(sk),.s_axis_tlast(sl),.s_axis_tvalid(sv),.s_axis_tready(sr),
        .m_axis_tdata(md),.m_axis_tkeep(mk),.m_axis_tlast(ml),.m_axis_tvalid(mv),.m_axis_tready(mr),
        .axis_wr_data_count(wr_count),.almost_full(almost_full),.prog_full(prog_full));

    // Two scoreboards: serial-word expectation at the FIFO inlet and exact
    // accepted-beat ordering at its outlet, including every TKEEP/TLAST.
    reg [63:0] queue_data[0:19999];reg queue_last[0:19999];
    integer put=0,get=0,base_pair=0,packet_pairs=8,out_last=0;
    reg check_serial=0,in_held=0,out_held=0;
    reg check_startup_pending=1;
    integer pending_busy_cycles=0;
    reg [63:0] old_sd,old_md;reg old_sl,old_ml;
    function automatic [31:0] raw_pair(input integer i);
        reg [15:0] a,b;begin a=16'h1357+i;b=16'h9abc-i;raw_pair={b,a};end
    endfunction
    always @(posedge rx) begin
        if(!cap_rn) in_held=0;
        else begin
            // RX baseline acquisition may now outlast the native FIFO's busy
            // interval. Observe the actual FIFO interval independently of PHY
            // readiness, while still requiring the delivered ARM to stay queued.
            if(check_startup_pending && !sr) begin
                if(!dut.u_receive.arm_pending || dut.u_receive.state!=0 || sv)
                    $fatal(1,"ARM escaped pending while native FIFO was still reset-busy");
                pending_busy_cycles=pending_busy_cycles+1;
            end
            if(in_held && (!sv || sd!==old_sd || sl!==old_sl || sk!==8'hff))
                $fatal(1,"capture AXIS changed under FIFO backpressure");
            in_held=sv&&!sr;old_sd=sd;old_sl=sl;
            if(sv&&sr) begin
                if(put>=20000 || sk!==8'hff) $fatal(1,"inlet capacity/TKEEP");
                if(check_serial && (sd!=={raw_pair(base_pair+put*2+1),raw_pair(base_pair+put*2)} ||
                    sl!==((put%(packet_pairs/2))==(packet_pairs/2-1))))
                    $fatal(1,"serial inlet mismatch beat=%0d got=%h last=%b base=%0d state=%0d",put,sd,sl,base_pair,dut.u_receive.state);
                queue_data[put]=sd;queue_last[put]=sl;put=put+1;
            end
        end
    end
    always @(posedge sys) begin
        if(!cap_rn) out_held=0;
        else begin
            if(out_held && (!mv || md!==old_md || ml!==old_ml || mk!==8'hff))
                $fatal(1,"FIFO outlet changed while DMA sink was stalled");
            out_held=mv&&!mr;old_md=md;old_ml=ml;
            if(mv&&!mr) stall_cycles=stall_cycles+1;
            if(mv&&mr) begin
                if(get>=put || md!==queue_data[get] || ml!==queue_last[get] || mk!==8'hff)
                    $fatal(1,"FIFO ordering/data/TKEEP/TLAST mismatch read=%0d write=%0d data=%h",get,put,md);
                if(ml) out_last=out_last+1;
                get=get+1;
            end
        end
    end
    always @(negedge sys) if(random_sink) begin
        rng={rng[14:0],rng[15]^rng[13]^rng[12]^rng[10]};mr=rng[0]||rng[3];
    end
    task command(input [6:0] op,input [7:0] idx,input [15:0] arg,input [5:0] code);
        @(negedge sys);seq=~seq;ctrl={seq,op,idx,arg};
        @(posedge sys);#0.1;
        if(status[31]!==seq || status[29:24]!==code || status[30]!=(code!=0))
            $fatal(1,"command op=%h index=%h expected=%0d status=%h fault=%h",op,idx,code,status,dut.sys_faults);
    endtask
    task configure(input integer length,input integer count,input integer origin);
        wait(!dut.cmd_busy && !dut.recover_active);@(negedge sys);
        packet_pairs=length;base_pair=origin;check_serial=1;
        command(2,0,length[15:0],0);command(3,0,length[31:16],0);
        command(2,1,count[15:0],0);command(3,1,count[31:16],0);
        command(2,4,16'h0008,0);command(3,4,16'h1616,0);
    endtask
    task wait_state(input integer value,input integer max_sys_cycles);
        integer age;begin age=0;
        while(dut.u_receive.state!==value && age<max_sys_cycles) begin @(negedge sys);age=age+1;end
        if(dut.u_receive.state!==value)
            $fatal(1,"state timeout wanted=%0d got=%0d sysfault=%h rxfault=%h",value,dut.u_receive.state,dut.sys_faults,dut.u_receive.faults);
        // The native XPM FIFO still has reset-busy latency AFTER its reset
        // input releases. Supply a real idle-clock preamble through TREADY.
        if(value==2) begin
            while(sr!==1'b1 && age<max_sys_cycles) begin @(negedge sys);age=age+1;end
            if(sr!==1'b1) $fatal(1,"official FIFO did not finish reset busy");
        end
        end
    endtask
    task bitcell(input bit f,input bit a,input bit b);
        fr=f;d0=a;d1=b;#UI;
    endtask
    task serial_words(input integer origin,input integer count);
        reg [31:0] pair;begin
        @(negedge bclk);#0.833;
        for(integer i=0;i<count;i=i+1) begin
            pair=raw_pair(origin+i);
            for(integer j=0;j<16;j=j+1) bitcell(j<8,pair[15-j],pair[31-j]);
        end
        fr=0;d0=0;d1=0;
        end
    endtask
    task drain(input integer expected,input integer expected_last);
        integer age;begin random_sink=1;age=0;
        while(get<expected && age<100000) begin @(negedge sys);age=age+1;end
        random_sink=0;mr=1;repeat(30) @(negedge sys);
        if(get!=expected || put!=expected || mv || out_last!=expected_last)
            $fatal(1,"drain mismatch inlet=%0d outlet=%0d expected=%0d TLAST=%0d expectedTLAST=%0d",put,get,expected,out_last,expected_last);
        end
    endtask
    task recover_case;
        // Firmware contract modeled here: producer is stopped at idle, DMA
        // sink is quiesced/reset, then explicit RECOVER token is issued.
        fr=0;d0=0;d1=0;random_sink=0;mr=0;dma_rn=0;
        repeat(10) @(negedge sys);command(8,0,16'hc1ea,0);
        wait(!dut.recover_active);repeat(5) @(negedge sys);
        put=0;get=0;out_last=0;in_held=0;out_held=0;check_serial=0;
        dma_rn=1;bitclock_run=1;
        wait(cap_rn);repeat(100) @(negedge sys);
        if(mv || dut.sys_faults || dut.session_open) $fatal(1,"RECOVER left stale FIFO/session/fault");
    endtask

    integer saved_put;
    always @(posedge cap_rn) $display("TRACE capture_resetn released t=%0t ready=%b count=%h",$time,sr,wr_count);
    always @(posedge sr) $display("TRACE FIFO ready t=%0t count=%h",$time,wr_count);
    always @(posedge dut.rx_fault) $display("TRACE RX fault t=%0t put=%0d sr=%b count=%h fault=%h",$time,put,sr,wr_count,dut.u_receive.faults);
    initial begin
        #1000;rn=1;#100;rr=0;wait(dut.arm_admissible_sys);repeat(20) @(negedge sys);
        command(1,0,3,0);configure(8,8,0);command(5,0,0,0);
        // ARM is acknowledged while no physical BITCLK edges exist.
        repeat(100) @(negedge sys);
        if(cap_rn || put || dut.sys_faults || !dut.session_open) $fatal(1,"ARM before BITCLK");
        bitclock_run=1;wait_state(2,1000);check_startup_pending=0;
        if(pending_busy_cycles<8) $fatal(1,"native FIFO reset-busy pending coverage missing");
        serial_words(0,64);wait_state(4,1000);repeat(100) @(negedge sys);
        if(put!=32 || get!=0 || !mv || dut.sys_faults) $fatal(1,"finite capture before FIFO drain");
        drain(32,8);
        $display("CASE PASS ARM-before-BITCLK + idle preamble + finite capture + FIFO held/drain");

        // Data begin simultaneously with BITCLK, before reset release/ARM.
        // No complete pre-ready burst is claimed; the next idle is mandatory.
        recover_case();bitclock_run=0;#20;dma_rn=0;#100;dma_rn=1;
        put=0;get=0;out_last=0;configure(8,1,200);command(5,0,0,0);
        check_startup_pending=1;bitclock_run=1;serial_words(1000,128);check_startup_pending=0;
        if(put || dut.rx_first || dut.sys_faults) $fatal(1,"pre-ready no-idle burst claimed");
        wait_state(2,1000);serial_words(200,8);wait_state(4,1000);drain(4,1);
        $display("CASE PASS no-idle pre-ready burst excluded; next idle-qualified burst captured");

        recover_case();configure(8,0,250);command(5,0,0,0);wait_state(2,1000);
        serial_words(250,3);repeat(30) @(negedge sys);command(6,0,0,0);
        repeat(30) @(negedge sys);
        if(dut.rx_complete || put!=1) $fatal(1,"graceful STOP ended before actual chunk boundary");
        serial_words(253,5);wait_state(4,1000);drain(4,1);
        $display("CASE PASS STOP-half finishes only after remaining real pairs reach boundary");

        // STOP in the middle of a chunk cannot invent padding/TLAST/completion.
        recover_case();configure(8,0,300);command(5,0,0,0);wait_state(2,1000);
        serial_words(300,3);repeat(30) @(negedge sys);command(6,0,0,0);
        repeat(100) @(negedge sys);
        if(dut.rx_complete || put!=1 || out_last || dut.sys_faults) $fatal(1,"STOP half packet fabricated completion");
        command(7,0,0,0);wait_state(5,1000);
        if(!dut.sys_faults[8] || !dut.u_receive.faults[2]) $fatal(1,"ABORT did not invalidate STOP half packet");
        drain(1,0);$display("CASE PASS STOP-half remains incomplete; ABORT retains only real beats");

        // A stopped physical clock is detected in SYS while the FIFO read side
        // remains alive. Restart alone cannot silently re-arm a failed session.
        recover_case();configure(16,0,400);command(5,0,0,0);wait_state(2,1000);
        serial_words(400,4);repeat(300) @(negedge sys);
        @(negedge bclk);bitclock_run=0;repeat(1000) @(negedge sys);
        if(!dut.sys_faults[3]) $fatal(1,"stopped BITCLK not detected");
        drain(2,0);bitclock_run=1;wait_state(5,1000);
        if(dut.rx_complete) $fatal(1,"clock restart claimed success");
        recover_case();configure(8,1,500);command(5,0,0,0);wait_state(2,1000);
        serial_words(500,8);wait_state(4,1000);drain(4,1);
        $display("CASE PASS stop/restart BITCLK; sticky fault; explicit RECOVER recaptures");

        // Stop after the first physical pair, before heartbeat_counter[7] has
        // ever toggled. rx_first itself must establish that capture had begun.
        recover_case();bitclock_run=0;#20;dma_rn=0;#100;dma_rn=1;
        configure(16,0,550);command(5,0,0,0);bitclock_run=1;wait_state(2,1000);
        serial_words(550,1);wait(dut.first_sys);@(negedge bclk);bitclock_run=0;
        if(dut.heartbeat_counter>=128 || dut.u_control.session_saw_clock)
            $fatal(1,"early-clock-stop test missed first-heartbeat window counter=%0d",dut.heartbeat_counter);
        repeat(1000) @(negedge sys);
        if(!dut.sys_faults[3] || dut.rx_complete || put)
            $fatal(1,"pre-heartbeat clock stop escaped watchdog");
        bitclock_run=1;wait_state(5,1000);
        $display("CASE PASS first pair then clock stop before first heartbeat");

        // DMA reset clears actual FIFO storage and RX state even before its
        // read side drains. A new ARM requires the explicit RECOVER contract.
        recover_case();configure(16,0,600);command(5,0,0,0);wait_state(2,1000);
        serial_words(600,6);repeat(100) @(negedge sys);
        if(put!=3 || !mv) $fatal(1,"DMA reset setup");
        dma_rn=0;repeat(30) @(negedge sys);
        if(cap_rn || !dut.sys_faults[4]) $fatal(1,"DMA reset not propagated/latching");
        command(5,0,0,7);recover_case();configure(8,1,700);command(5,0,0,0);wait_state(2,1000);
        serial_words(700,8);wait_state(4,1000);drain(4,1);
        $display("CASE PASS DMA reset discards old FIFO epoch; RECOVER has no stale output");

        // Fill the actual 8192-deep AMD FIFO with a non-backpressurable serial
        // source. The final held beat is immutable and no false TLAST is made.
        recover_case();configure(64,0,2000);command(5,0,0,0);wait_state(2,1000);
        serial_words(2000,17000);wait_state(5,1000);repeat(100) @(negedge sys);
        if(!dut.u_receive.faults[0] || !dut.u_receive.prog_full_seen || !dut.u_receive.almost_full_seen ||
            !sv || sr || !mv || put<8190 || put>8200 || wr_count<8190)
            $fatal(1,"real FIFO full/overflow mismatch in=%0d count=%0d fault=%h flags=%b%b sr=%b",put,wr_count,dut.u_receive.faults,dut.u_receive.prog_full_seen,dut.u_receive.almost_full_seen,sr);
        saved_put=put;repeat(100) @(negedge sys);
        if(put!=saved_put || dut.rx_complete) $fatal(1,"overflow invented completion or overwrite");
        // Exactly one already offered beat may drain after FIFO has space.
        random_sink=1;wait(!sv);repeat(20) @(negedge sys);saved_put=put;
        drain(saved_put,saved_put/32);
        if(dut.u_receive.state!=5 || !dut.sys_faults[7] || saved_put<8192)
            $fatal(1,"overflow lost its sticky fault after drain");
        $display("CASE PASS real FIFO full/prog_full/almost_full; %0d beats retained and drained",saved_put);
        recover_case();configure(8,1,800);command(5,0,0,0);wait_state(2,1000);
        serial_words(800,8);wait_state(4,1000);drain(4,1);
        if(stall_cycles<100) $fatal(1,"outlet stall coverage missing");
        $display("PASS tb_capture_fifo: real serial UNISIM + official async8192 FIFO; ARM/reset-busy-pending/idle/no-idle/STOP/ABORT/clockstop/DMAreset/RECOVER/full/scoreboard; stalled sink cycles=%0d pending cycles=%0d",stall_cycles,pending_busy_cycles);
        $finish;
    end
    initial begin #5000000;$fatal(1,"capture/FIFO regression timeout state=%0d sysfault=%h",dut.u_receive.state,dut.sys_faults);end
endmodule
