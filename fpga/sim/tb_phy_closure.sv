`timescale 1ns/1ps
// Actual AMD UNISIM PHY, bounded SYS deadlines. No project writer or hardware.
module tb_phy_closure;
    reg sys=0,refclk=0,bclk=0,run=0,rn=0,rr=1,cancel=0,send=0;
    reg sys_run=1,ref_run=1;
    reg [27:0] cfg=0;
    always #5 if(sys_run) sys=~sys;
    initial begin #0.193;forever begin #1.666667;if(ref_run) refclk=~refclk;end end
    initial begin #0.371;forever begin #1.666667;if(run) bclk=~bclk;end end
    wire busy,ready,arm,calready,bvalid,rx,des,tvalid,refready;
    wire [3:0] result,fault;wire [2:0] phase;wire [31:0] generation,bgen,tgen;
    wire [26:0] baseline,taps,rxbase;
    integer loads=0,terminals=0;
    radar_lvds_phy #(.DELAY_TIMEOUT_SYS_CYCLES(3000),.CAL_TIMEOUT_SYS_CYCLES(2000),
        .CAL_RDY_TIMEOUT_CYCLES(3000)) phy(
        .sys_clk(sys),.sys_resetn(rn),.ref_clk(refclk),.ref_reset(rr),
        .cfg_send(send),.cfg_data(cfg),.cancel_sys(cancel),.cfg_busy(busy),.ready_sys(ready),
        .arm_admissible_sys(arm),.ref_cal_ready_sys(calready),.rx_ready_sys(),
        .baseline_valid_sys(bvalid),.baseline_data(baseline),.baseline_generation_sys(bgen),
        .cfg_result(result),.phy_fault_sys(fault),.cfg_phase(phase),.cfg_generation(generation),
        .bitclk_p(bclk),.bitclk_n(~bclk),.frclk_p(1'b0),.frclk_n(1'b1),
        .data0_p(1'b0),.data0_n(1'b1),.data1_p(1'b0),.data1_n(1'b1),
        .rx_clk(rx),.deser_resetn(des),.fr_byte(),.data0_byte(),.data1_byte(),
        .delay_readback(taps),.baseline_readback_rx(rxbase),.tap_valid_rx(tvalid),
        .tap_generation_rx(tgen),.ref_ready(refready));
    always @(posedge rx) if(rn && phy.delay_load!=0) loads=loads+1;
    // Native primitive pins, not just FSM metadata. Configuration INIT is the
    // only exception; runtime RST edges must occur with both clocks held LOW.
    realtime primitive_asserted=0,primitive_released=0,controller_released=0;
    realtime first_serdes_edge=0,first_rx_edge=0,gate_opened=0;
    realtime serdes_rise=0,serdes_fall=0,rx_rise=0,rx_fall=0;
    integer ordered_resets=0,vtc_high_refs=0,full_edges=0;
    reg first_serdes_pending=0,first_rx_pending=0;
    always @(posedge refclk) begin
        if(phy.en_vtc) vtc_high_refs=vtc_high_refs+1;else vtc_high_refs=0;
    end
    always @(posedge phy.primitive_reset) if($time>1000) begin
        primitive_asserted=$realtime;
        if(phy.clock_enable_ref || phy.bitclk_resetn || phy.serdes_clk || rx ||
           !phy.en_vtc || vtc_high_refs<8)
            $fatal(1,"REGRESSION_ASSERT primitive reset asserted before clocks off/VTC guard");
    end
    always @(negedge phy.primitive_reset) begin
        primitive_released=$realtime;ordered_resets=ordered_resets+1;
        if(!phy.ctl_reset || phy.bitclk_resetn || phy.serdes_clk || rx || !phy.en_vtc ||
           $realtime-primitive_asserted<213.0)
            $fatal(1,"REGRESSION_ASSERT primitive reset release/order/64REF hold");
    end
    always @(negedge phy.ctl_reset) begin
        controller_released=$realtime;
        if(phy.primitive_reset || phy.bitclk_resetn || phy.serdes_clk || rx ||
           $realtime-primitive_released<53.0)
            $fatal(1,"REGRESSION_ASSERT controller reset not after primitive reset 16REF guard");
    end
    always @(posedge phy.clock_enable_ref) begin
        full_edges=0;
        if(phy.primitive_reset || phy.ctl_reset || !phy.idelay_rdy ||
           $realtime-controller_released<53.0)
            $fatal(1,"REGRESSION_ASSERT clock permission before primitive/CTRL calibration");
    end
    always @(posedge phy.bit_clk_global) if(phy.clock_enable_ref) full_edges=full_edges+1;
    always @(posedge phy.bitclk_resetn) begin
        gate_opened=$realtime;first_serdes_pending=1;first_rx_pending=1;
        if(!phy.clock_enable_ref || full_edges<7 || phy.primitive_reset || phy.ctl_reset)
            $fatal(1,"REGRESSION_ASSERT shared CLR lacks full BITCLK qualification");
    end
    always @(posedge phy.serdes_clk) if(first_serdes_pending) begin
        first_serdes_edge=$realtime;first_serdes_pending=0;
        #0.02;if(first_rx_pending || first_rx_edge!=first_serdes_edge)
            $fatal(1,"REGRESSION_ASSERT /1 and /4 first active edges misaligned");
    end
    always @(posedge rx) if(first_rx_pending) begin first_rx_edge=$realtime;first_rx_pending=0;end
    always @(negedge phy.bitclk_resetn) begin
        serdes_rise=0;serdes_fall=0;rx_rise=0;rx_fall=0;
        #0.02;if(phy.serdes_clk!==0 || rx!==0)
            $fatal(1,"REGRESSION_ASSERT shared CLR failed to force stopped-high/low outputs LOW");
    end
    // CLR assertion may intentionally truncate a high pulse while data is
    // invalid. After release, both real clock buffers must emit full pulses.
    always @(posedge phy.serdes_clk) if(phy.bitclk_resetn) begin
        if(serdes_fall>gate_opened && $realtime-serdes_fall<1.60)
            $fatal(1,"REGRESSION_ASSERT shortened /1 LOW pulse after release");
        serdes_rise=$realtime;
    end
    always @(negedge phy.serdes_clk) if(phy.bitclk_resetn && phy.clock_enable_ref) begin
        if(serdes_rise>=gate_opened && $realtime-serdes_rise<1.60)
            $fatal(1,"REGRESSION_ASSERT shortened /1 HIGH pulse after release");
        serdes_fall=$realtime;
    end
    always @(posedge rx) if(phy.bitclk_resetn) begin
        if(rx_fall>gate_opened && $realtime-rx_fall<6.50)
            $fatal(1,"REGRESSION_ASSERT shortened /4 LOW pulse after release");
        rx_rise=$realtime;
    end
    always @(negedge rx) if(phy.bitclk_resetn && phy.clock_enable_ref) begin
        if(rx_rise>=gate_opened && $realtime-rx_rise<6.50)
            $fatal(1,"REGRESSION_ASSERT shortened /4 HIGH pulse after release");
        rx_fall=$realtime;
    end
    task sys_wait(input integer n);repeat(n) @(negedge sys);endtask
    task wait_arm;
        integer age;begin age=0;while(!arm && age<5000) begin sys_wait(1);age=age+1;end
        if(!arm) $fatal(1,"REGRESSION_ASSERT REF calibration did not allow ARM without RX");end
    endtask
    task wait_ready;
        integer age;begin age=0;while(!ready && age<5000) begin sys_wait(1);age=age+1;end
        if(!ready || !bvalid || !tvalid) $fatal(1,"REGRESSION_ASSERT RX preparation timeout");end
    endtask
    task request(input [27:0] value);
        wait(!busy);@(negedge sys);cfg=value;send=1;@(negedge sys);send=0;
    endtask
    task terminal(input [31:0] previous,input [3:0] expected);
        integer age;begin age=0;
        while(generation==previous && age<6000) begin sys_wait(1);age=age+1;end
        if(generation!=previous+1 || busy || result!==expected)
            $fatal(1,"REGRESSION_ASSERT terminal mismatch prev=%0d gen=%0d busy=%b code=%0d expected=%0d",previous,generation,busy,result,expected);
        sys_wait(5);if(generation!=previous+1) $fatal(1,"REGRESSION_ASSERT double terminal");
        end
    endtask
    task recal;
        reg [31:0] g;begin g=generation;request({1'b1,27'd0});
        if(fault!==0) $fatal(1,"REGRESSION_ASSERT accepted RECAL did not clear PHY fault");
        terminal(g,1);wait_arm();end
    endtask
    task wait_fault(input [3:0] expected);
        integer age;begin age=0;
        while(fault!=expected && age<6000) begin sys_wait(1);age=age+1;end
        if(fault!==expected) $fatal(1,"REGRESSION_ASSERT PHY fault expected=%0d actual=%0d",expected,fault);
        end
    endtask
    integer saved_loads;reg [31:0] old_gen,old_bgen,old_tgen;
    integer epoch_reset_sequence,epoch_new_requests=0;
    reg [31:0] epoch_owner_generation=0;
    reg epoch_probe=0;
    always @(posedge refclk) if(epoch_probe && phy.recal_valid)
        epoch_new_requests=epoch_new_requests+1;
    always @(posedge sys) if(epoch_probe && generation!=epoch_owner_generation &&
        (epoch_new_requests!=1 || ordered_resets<=epoch_reset_sequence))
        $fatal(1,"REGRESSION_ASSERT old REF report terminated new owner code=%0d requests=%0d old_sequence=%0d current_sequence=%0d",result,epoch_new_requests,epoch_reset_sequence,ordered_resets);
    initial begin
        // Automatic calibration failures need a diagnosis even though there
        // is no software owner and REF has never asserted ready.
        force phy.idelay_rdy=0;
        #1000;rn=1;#101.37;rr=0;wait_fault(9);
        if(generation || result || busy || arm || bvalid || bgen)
            $fatal(1,"REGRESSION_ASSERT boot RDY timeout invented software terminal/readiness");
        release phy.idelay_rdy;recal();
        if(fault || !arm || bvalid) $fatal(1,"REGRESSION_ASSERT boot timeout RECAL recovery");
        rn=0;sys_wait(5);rn=1;wait(phy.u_cal_ctrl.state==5);
        force phy.idelay_rdy=0;wait_fault(9);
        if(generation || result || busy || arm || bvalid || bgen)
            $fatal(1,"REGRESSION_ASSERT boot SETTLE loss invented software terminal/readiness");
        release phy.idelay_rdy;recal();
        // Supersede a boot-failure report while it is still entering its CDC
        // channel. Both REF endpoints must stay isolated for the new owner.
        rn=0;sys_wait(5);force phy.idelay_rdy=0;rn=1;
        wait(phy.u_cal_ctrl.report_pending);release phy.idelay_rdy;
        old_gen=generation;request({1'b1,27'd0});
        repeat(6) begin
            sys_wait(1);
            if(!busy || generation!=old_gen || phy.cal_epoch_resetn || fault)
                $fatal(1,"REGRESSION_ASSERT old boot report ended/reopened new RECAL during isolation");
        end
        terminal(old_gen,1);wait_arm();
        if(fault) $fatal(1,"REGRESSION_ASSERT delayed boot failure poisoned new RECAL");
        if(ready || des || bvalid || bgen || tvalid || loads) $fatal(1,"REGRESSION_ASSERT fabricated no-clock readiness/baseline");
        old_gen=generation;request(28'd42);terminal(old_gen,2);
        recal();if(bvalid || bgen || ready) $fatal(1,"REGRESSION_ASSERT no-clock RECAL fabricated observation");
        run=1;wait_ready();if(bgen!=1 || baseline!==0) $fatal(1,"REGRESSION_ASSERT first actual baseline");
        // REF completion is a software terminal; a later automatic RX baseline
        // failure has a separate sticky diagnosis, never a fabricated terminal.
        run=0;recal();old_gen=generation;old_bgen=bgen;old_tgen=tgen;saved_loads=loads;
        force phy.tap_count_live=27'bx;run=1;wait_fault(4);
        if(busy || ready || arm || bvalid || phy.rx_epoch_resetn || generation!=old_gen ||
           result!=1 || bgen!=old_bgen || tgen!=old_tgen || loads!=saved_loads)
            $fatal(1,"REGRESSION_ASSERT automatic baseline failure altered command or fabricated data");
        release phy.tap_count_live;sys_wait(100);
        if(fault!=4 || ready || arm) $fatal(1,"REGRESSION_ASSERT baseline failure diagnosis not sticky");
        recal();wait_ready();
        // An idle RDY failure is also visible without inventing a software
        // command or reusing the previous command's SUCCESS as PHY health.
        old_gen=generation;force phy.idelay_rdy=0;wait_fault(6);
        if(busy || ready || arm || generation!=old_gen || result!=1)
            $fatal(1,"REGRESSION_ASSERT idle RDY fault changed software terminal");
        release phy.idelay_rdy;sys_wait(100);
        if(fault!=6 || ready || arm) $fatal(1,"REGRESSION_ASSERT idle RDY fault not sticky");
        recal();wait_ready();
        old_gen=generation;request({1'b0,9'd53,9'd37,9'd21});terminal(old_gen,1);wait_ready();
        if(taps!=={9'd53,9'd37,9'd21}) $fatal(1,"REGRESSION_ASSERT native tap result");
        // Stop in every executor phase, alternating high/low. Half of cases
        // explicitly cancel, the others must expire in the continuous SYS domain.
        for(integer stage=1;stage<=10;stage=stage+1) begin
            recal();wait_ready();old_gen=generation;
            request({1'b0,9'd160,9'd120,9'd80});wait(phy.delay_state==stage);
            if(stage[0]) wait(bclk==1);else wait(bclk==0);
            run=0;
            if(stage[0]) begin sys_wait(3);cancel=1;sys_wait(1);cancel=0;end
            terminal(old_gen,stage[0]?8:7);
            if(phy.delay_load || ready || arm || phy.rx_epoch_resetn || bvalid)
                $fatal(1,"REGRESSION_ASSERT failed execution was not persistently isolated stage=%0d",stage);
            saved_loads=loads;old_bgen=bgen;run=1;sys_wait(300);
            if(loads!=saved_loads || ready || generation!=old_gen+1 || bgen!=old_bgen)
                $fatal(1,"REGRESSION_ASSERT stale LOAD/result/baseline replay stage=%0d",stage);
            run=0;old_tgen=tgen;recal();
            if(bvalid || ready || bgen!=old_bgen) $fatal(1,"REGRESSION_ASSERT RECAL without RX invented baseline");
            if(tgen!=old_tgen) $fatal(1,"REGRESSION_ASSERT no-clock RECAL changed measurement identity");
            saved_loads=loads;run=1;wait_ready();
            if(loads!=saved_loads || bgen!=old_bgen+1 || tgen!=old_tgen+1)
                $fatal(1,"REGRESSION_ASSERT epoch reopen replay/measurement generation");
        end
        // Cancel on the SYS edge that would consume an otherwise successful
        // report. Isolation must precede a single CANCELED terminal.
        old_gen=generation;request(28'd9);wait(phy.rx_report_valid);
        cancel=1;@(posedge sys);@(negedge sys);cancel=0;
        if(!busy || generation!=old_gen || phy.rx_epoch_resetn)
            $fatal(1,"REGRESSION_ASSERT same-edge cancel lost ownership/isolation priority");
        terminal(old_gen,8);recal();wait_ready();
        // RDY loss and timeout must leave explicit RECAL usable.
        old_gen=generation;request(28'd42);wait(phy.delay_state==1);force phy.idelay_rdy=0;
        terminal(old_gen,6);release phy.idelay_rdy;sys_wait(100);
        if(ready || arm) $fatal(1,"REGRESSION_ASSERT RDY return auto recovered");
        recal();wait_ready();
        force phy.idelay_rdy=0;old_gen=generation;request({1'b1,27'd0});terminal(old_gen,9);
        release phy.idelay_rdy;recal();wait_ready();
        // One complete new transaction after all cancels proves old ACKs cannot
        // terminate a new owner. The actual requested taps must be observed.
        old_gen=generation;request({1'b0,9'd9,9'd8,9'd1});terminal(old_gen,1);wait_ready();
        if(taps!=={9'd9,9'd8,9'd1}) $fatal(1,"REGRESSION_ASSERT old completion ended new command");
        // Divider release depends on true full BITCLK; test reset while stopped
        // high and low, and withdrawal at two unrelated SYS/BITCLK phases.
        for(integer polarity=0;polarity<2;polarity=polarity+1) begin
            wait(bclk==polarity);run=0;rn=0;sys_wait(5);
            if(rx || phy.serdes_clk || des || phy.bitclk_resetn) $fatal(1,"REGRESSION_ASSERT divider asynchronous assertion");
            #(polarity?1.213:3.117);rn=1;sys_wait(20);
            if(rx || phy.serdes_clk || phy.bitclk_resetn) $fatal(1,"REGRESSION_ASSERT divider released without BITCLK");
            run=1;wait_arm();wait_ready();
        end
        // REF reset is an independent asynchronous source. Qualify it in SYS
        // first; even a stopped REF clock must withdraw permission promptly,
        // and must not release REF/clock outputs until REF actually returns.
        for(integer polarity=0;polarity<2;polarity=polarity+1) begin
            wait(refclk==polarity);ref_run=0;
            wait(bclk==polarity);run=0;
            #(polarity?0.417:2.319);rr=1;sys_wait(3);
            if(phy.phy_sys_resetn || phy.ref_resetn || arm || ready ||
               phy.bitclk_resetn || phy.serdes_clk || rx)
                $fatal(1,"REGRESSION_ASSERT independent REF reset failed SYS ownership/safe assertion");
            #(polarity?1.117:0.731);rr=0;sys_wait(30);
            if(phy.ref_resetn || arm || ready || phy.bitclk_resetn || rx)
                $fatal(1,"REGRESSION_ASSERT reset released with REF stopped");
            ref_run=1;wait_arm();
            if(phy.bitclk_resetn || phy.serdes_clk || rx || ready)
                $fatal(1,"REGRESSION_ASSERT REF recovery fabricated BITCLK activity");
            run=1;wait_ready();
        end
        // The separate SYS owner retains asynchronous SYS-reset assertion.
        // Stop every clock in both polarities, assert SYS reset between edges,
        // then prove neither owner nor PHY can release without a real SYS edge.
        for(integer polarity=0;polarity<2;polarity=polarity+1) begin
            wait(sys==polarity);sys_run=0;
            wait(refclk==polarity);ref_run=0;
            wait(bclk==polarity);run=0;
            #(polarity?1.137:0.719);rn=0;#50;
            if(phy.phy_allow_sys || phy.phy_sys_resetn || phy.ref_resetn ||
               phy.bitclk_resetn || phy.serdes_clk || rx || arm || ready)
                $fatal(1,"REGRESSION_ASSERT SYS reset not asynchronous with all clocks stopped");
            rn=1;#50;
            if(phy.phy_allow_sys || phy.phy_sys_resetn || phy.bitclk_resetn || rx)
                $fatal(1,"REGRESSION_ASSERT reset ownership released without SYS clock");
            sys_run=1;ref_run=1;wait_arm();run=1;wait_ready();
        end
        // A low SYS epoch must be remembered even when REF cannot sample it.
        // Freeze an old explicit RECAL at its final SETTLE count, cancel it,
        // and submit a new RECAL while REF is still stopped. The old success
        // must not complete the new owner before its own reset/calibration.
        run=0;old_gen=generation;request({1'b1,27'd0});
        wait(phy.u_cal_ctrl.state==5 && phy.u_cal_ctrl.timer==15);
        // Isolate the digital epoch race from IDELAYCTRL's separate stopped-
        // REF RDY watchdog (already covered above). Release the injection as
        // soon as the new request really reaches its own primitive reset.
        @(negedge refclk);force phy.idelay_rdy=1;ref_run=0;
        cancel=1;@(posedge sys);@(negedge sys);cancel=0;terminal(old_gen,8);
        epoch_reset_sequence=ordered_resets;epoch_new_requests=0;
        epoch_owner_generation=generation;epoch_probe=1;
        old_gen=generation;request({1'b1,27'd0});sys_wait(30);
        if(!busy || generation!=old_gen)
            $fatal(1,"REGRESSION_ASSERT stopped REF completed a new RECAL");
        ref_run=1;
        fork begin wait(phy.recal_valid);wait(phy.primitive_reset);release phy.idelay_rdy;end join_none
        terminal(old_gen,1);
        if(epoch_new_requests!=1 || ordered_resets<=epoch_reset_sequence || !refready)
            $fatal(1,"REGRESSION_ASSERT stale REF success completed new RECAL requests=%0d old_sequence=%0d current_sequence=%0d refready=%b",epoch_new_requests,epoch_reset_sequence,ordered_resets,refready);
        epoch_probe=0;wait_arm();run=1;wait_ready();
        if(ordered_resets<20) $fatal(1,"REGRESSION_ASSERT reset order scenarios not exercised");
        $display("PASS tb_phy_closure: native RX-clocked IDELAY, no-clock ARM/DELAY/RECAL, every-phase stop-high/low timeout/cancel, no replay, independent fault diagnosis, ordered primitive/CTRL reset, isolated REF/SYS reset ownership and qualified /1+/4 clocks (%0d sequences)",ordered_resets);$finish;
    end
    initial begin #5000000;$fatal(1,"REGRESSION_ASSERT total SYS deadline");end
endmodule
