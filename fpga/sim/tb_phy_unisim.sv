`timescale 1ns/1ps
// Optional AMD UNISIM test. Only the real vendor models are used for the PHY;
// this distribution intentionally does not substitute zero-delay fake primitives.
module tb_phy_unisim;
    reg sys=0,refclk=0,bclk=0;
    always #5 sys=~sys;
    initial begin #0.193;forever #1.667 refclk=~refclk;end
    initial begin #0.371;forever #1.667 bclk=~bclk;end
    reg rn=0,rr=1,fr=0,d0=0,d1=0,cfg_send=0;
    reg [27:0] cfg_data=0;
    wire rc,des,ready,busy,refready;
    wire [7:0] fb,b0,b1;wire [26:0] taps;
    wire [26:0] native_taps={phy.g_lane[2].u_delay.CNTVALUEOUT,
        phy.g_lane[1].u_delay.CNTVALUEOUT,phy.g_lane[0].u_delay.CNTVALUEOUT};
    integer tap_checks=0;
    wire [31:0] pdata,words,errs;wire pv,first,fe,idle;wire [3:0] phase;
    integer got=0,expected_base=0,expected_count=32,total=0;reg scoring=0;
    radar_lvds_phy phy(.sys_clk(sys),.sys_resetn(rn),.ref_clk(refclk),.ref_reset(rr),
        .cfg_send(cfg_send),.cfg_data(cfg_data),.cancel_sys(1'b0),.cfg_busy(busy),.ready_sys(ready),
        .bitclk_p(bclk),.bitclk_n(~bclk),.frclk_p(fr),.frclk_n(~fr),
        .data0_p(d0),.data0_n(~d0),.data1_p(d1),.data1_n(~d1),
        .rx_clk(rc),.deser_resetn(des),.fr_byte(fb),.data0_byte(b0),.data1_byte(b1),
        .delay_readback(taps),.ref_ready(refready));
    radar_fclk16_decode dec(.clk(rc),.resetn(des),.cfg(5'b01000),.fr_bits(fb),.d0_bits(b0),.d1_bits(b1),
        .pair_data(pdata),.pair_valid(pv),.burst_first(first),.framing_error(fe),
        .idle_gap(idle),.phase(phase),.words_seen(words),.errors_seen(errs));
    always @(posedge rc) begin
        #0.01;if(scoring && pv) begin
            if(got>=expected_count || pdata[15:0]!==16'(16'h1357+expected_base+got) ||
               pdata[31:16]!==16'(16'h9abc-expected_base-got))
                $fatal(1,"UNISIM PHY order/phase mismatch word=%0d got=%h",got,pdata);
            got=got+1;
        end
        if(scoring && fe) $fatal(1,"UNISIM frame pattern error");
    end
    task bitcell(input bit f,input bit a,input bit b);
        // Change at the center between BITCLK sample edges, then hold for one UI.
        fr=f;d0=a;d1=b;#1.667;
    endtask
    task burst(input integer origin,input integer count,input realtime launch_phase);
        reg [15:0] a,b;begin
        got=0;expected_base=origin;expected_count=count;
        @(negedge bclk);#(launch_phase);scoring=1;
        for(integer i=0;i<count;i=i+1) begin
            a=16'h1357+origin+i;b=16'h9abc-origin-i;
            for(integer j=0;j<16;j=j+1) bitcell(j<8,a[15-j],b[15-j]);
        end
        repeat(128) bitcell(0,0,0);
        #1000;if(got!=count) $fatal(1,"UNISIM word count got=%0d expected=%0d",got,count);
        scoring=0;total=total+got;
        end
    endtask
    task configure(input [27:0] value);
        integer cycles;begin
        tap_checks=0;
        wait(!busy);@(negedge sys);cfg_data=value;cfg_send=1;
        @(negedge sys);cfg_send=0;
        if(!busy) $fatal(1,"PHY configuration ownership not latched");
        cycles=0;
        while(busy && cycles<10000) begin @(negedge sys);cycles=cycles+1;end
        if(busy || !refready) $fatal(1,"PHY config ended before settled/calibrated");
        wait(ready);
        if(!value[27] && tap_checks==0) $fatal(1,"no valid post-LOAD tap readback window");
        if((^taps)===1'bx || (!value[27] && taps!==value[26:0]))
            $fatal(1,"settled PHY tap snapshot invalid/mismatched got=%h requested=%h",taps,value[26:0]);
        wait(des);#300;
        end
    endtask
    always @(posedge rc) begin
        for(integer lane=0;lane<3;lane=lane+1) if(phy.delay_load[lane]) begin
            if(phy.en_vtc || (^native_taps[lane*9+:9])===1'bx ||
               (integer'(phy.tap_values[lane*9+:9])-integer'(native_taps[lane*9+:9]))>8 ||
               (integer'(phy.tap_values[lane*9+:9])-integer'(native_taps[lane*9+:9])) < -8)
                $fatal(1,"native per-lane LOAD exceeded legal step/window");
            tap_checks=tap_checks+1;
        end
    end
    initial begin
        #1000;rn=1;#100;rr=0;wait(ready);wait(des);#1000;
        burst(0,32,0.833);
        // Independent SYS/REF/BITCLK phase plus three distinct nonzero taps;
        // taps pass through real IDELAYE3 LOAD/CNTVALUEOUT and serial data path.
        configure({1'b0,9'd53,9'd37,9'd21});
        burst(100,48,0.583);
        // Explicit digital RDY-fault injection; it is not an analog clock-loss
        // model. All vendor primitives remain instantiated and otherwise real.
        force phy.idelay_rdy=1'b0;#100;
        if(ready || refready || des || phy.rx_epoch_resetn) $fatal(1,"RDY loss not fail-closed");
        release phy.idelay_rdy;#1000;
        if(ready || refready || des || phy.rx_epoch_resetn) $fatal(1,"RDY return silently rearmed PHY");
        configure({1'b1,27'd0});
        configure({1'b0,9'd17,9'd11,9'd7});
        burst(200,64,1.083);
        $display("PASS tb_phy_unisim: %0d RAW16 pairs; unrelated phases; two nonzero tap sets; injected RDY-loss sticky fault; explicit RECAL recovery",total);$finish;
    end
    initial begin #1000000;$fatal(1,"UNISIM calibration or data timeout");end
endmodule
