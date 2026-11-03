`timescale 1ns/1ps
module tb_packetizer;
    reg clk=0;always #5 clk=~clk;
    reg rn=0,arm=0,stop=0,abort_c=0,idle=1,frerr=0,pv=0,ready=1;
    reg [31:0] n=4,limit_n=3,skip_n=0,sid=11,pd=0;
    wire [63:0] data;wire [7:0] keep;wire v,last;wire [2:0] state;wire [15:0] faults;
    wire [63:0] pairs,beats;wire [31:0] packets,peak,session;
    wire first,prog,almost;
    integer got=0,expected_beats=0;
    reg score=0;
    reg random_ready=0;
    reg [15:0] ready_rng=16'h4d2b;
    integer low_run=0, random_stalls=0;
    // Deterministic random stalls, bounded to two cycles so the fixed-rate
    // one-pair/two-cycle source is not deliberately overrun in this case.
    always @(negedge clk) if(random_ready) begin
        ready_rng={ready_rng[14:0],ready_rng[15]^ready_rng[13]^ready_rng[12]^ready_rng[10]};
        if(low_run==2 || ready_rng[0]) begin ready=1;low_run=0;end
        else begin ready=0;low_run=low_run+1;random_stalls=random_stalls+1;end
    end
    reg [63:0] old_data; reg old_last,was_stalled=0;
    radar_raw_packetizer dut(.clk(clk),.resetn(rn),.arm(arm),.stop(stop),.abort_capture(abort_c),
        .packet_pairs(n),.packet_limit(limit_n),.skip_pairs(skip_n),.session_id(sid),
        .idle_gap(idle),.framing_error(frerr),.pair_valid(pv),.pair_data(pd),
        .fifo_count(32'd128),.fifo_prog_full(1'b0),.fifo_almost_full(1'b0),
        .m_tdata(data),.m_tkeep(keep),.m_tvalid(v),.m_tready(ready),.m_tlast(last),
        .state(state),.faults(faults),.accepted_pairs(pairs),.accepted_beats(beats),
        .completed_packets(packets),.peak_fifo(peak),.current_session(session),
        .first_data_seen(first),.prog_full_seen(prog),.almost_full_seen(almost));
    always @(posedge clk) begin
        if(rn) begin
            if(was_stalled && (!v || data!==old_data || last!==old_last || keep!==8'hff))
                $fatal(1,"AXIS changed under backpressure");
            was_stalled=v&&!ready;old_data=data;old_last=last;
            if(score && v && ready) begin
                if(data[31:0]!==32'(got*2+skip_n) || data[63:32]!==32'(got*2+1+skip_n))
                    $fatal(1,"packet data mismatch beat %0d data %h",got,data);
                if(last!==((got%(n/2))==(n/2-1))) $fatal(1,"TLAST location");
                got=got+1;
            end
        end else was_stalled=0;
    end
    task reset_case;
        random_ready=0;rn=0;arm=0;pv=0;stop=0;abort_c=0;ready=1;score=0;got=0;
        idle=1;frerr=0;low_run=0;
        repeat(4) @(negedge clk);rn=1;repeat(2) @(negedge clk);
    endtask
    task start_case;
        @(negedge clk);arm=1;@(negedge clk);arm=0;
    endtask
    task pair_word(input integer value);
        @(negedge clk);pd=value;pv=1;@(negedge clk);pv=0;
    endtask
    initial begin
        reset_case();n=4;limit_n=3;skip_n=0;score=1;start_case();
        for(integer i=0;i<12;i=i+1) pair_word(i);
        repeat(6) @(negedge clk);
        if(state!=4 || faults || got!=6 || packets!=3 || pairs!=12 || beats!=6) $fatal(1,"finite capture");
        // Skip exactly two raw lane pairs before transport packaging.
        reset_case();n=4;limit_n=1;skip_n=2;score=1;start_case();
        for(integer i=0;i<6;i=i+1) pair_word(i);
        repeat(6) @(negedge clk);
        if(got!=2 || state!=4) $fatal(1,"skip pair count");
        // A completed output beat remains stable through 20 stall cycles.
        reset_case();n=4;limit_n=1;skip_n=0;ready=0;start_case();
        pair_word(0);pair_word(1);pair_word(2);
        repeat(20) @(negedge clk);
        if(!v || faults) $fatal(1,"stall storage");
        ready=1;score=1;pair_word(3);repeat(6) @(negedge clk);
        if(got!=2 || state!=4) $fatal(1,"resume after stall");
        // Non-backpressurable input overflows without overwriting the pending beat.
        reset_case();n=16;limit_n=0;ready=0;start_case();
        pair_word(0);pair_word(1);pair_word(2);pair_word(3);
        repeat(3) @(negedge clk);
        if(state!=5 || !faults[0] || !v || data!==64'h0000000100000000 || last) $fatal(1,"overflow not safely latched");
        repeat(20) @(negedge clk);ready=1;repeat(4) @(negedge clk);
        if(v || packets!=0) $fatal(1,"invented truncated packet");
        reset_case();n=3;limit_n=1;start_case();repeat(3) @(negedge clk);
        if(!faults[4] || state!=5 || v) $fatal(1,"odd byte count accepted");
        reset_case();n=4;limit_n=0;start_case();
        @(negedge clk);stop=1;@(negedge clk);stop=0;repeat(4) @(negedge clk);
        if(state!=4 || packets!=0) $fatal(1,"empty boundary stop");
        reset_case();n=4;limit_n=0;score=1;start_case();pair_word(0);
        @(negedge clk);stop=1;@(negedge clk);stop=0;
        pair_word(1);pair_word(2);pair_word(3);repeat(6) @(negedge clk);
        if(state!=4 || packets!=1 || got!=2) $fatal(1,"mid-chunk graceful stop");
        reset_case();n=4;limit_n=0;start_case();pair_word(0);
        @(negedge clk);frerr=1;@(negedge clk);frerr=0;repeat(2) @(negedge clk);
        if(state!=5 || !faults[1]) $fatal(1,"framing fault");
        // Many packet boundaries under randomized, bounded backpressure.
        reset_case();n=8;limit_n=16;skip_n=0;score=1;start_case();random_ready=1;
        for(integer i=0;i<128;i=i+1) pair_word(i);
        repeat(10) @(negedge clk);random_ready=0;ready=1;
        repeat(5) @(negedge clk);
        if(state!=4 || faults || got!=64 || packets!=16 || random_stalls<10)
            $fatal(1,"random stall regression got=%0d packets=%0d faults=%h stalls=%0d",got,packets,faults,random_stalls);
        // ABORT invalidates the session without retracting an already offered
        // non-last beat. Subsequent raw input must not overwrite that beat.
        reset_case();n=8;limit_n=0;skip_n=0;ready=0;start_case();
        pair_word(0);pair_word(1);
        @(negedge clk);abort_c=1;@(negedge clk);abort_c=0;
        for(integer i=2;i<8;i=i+1) pair_word(i);
        repeat(10) @(negedge clk);
        if(state!=5 || !faults[2] || !v || data!==64'h0000000100000000 || last)
            $fatal(1,"ABORT replaced/retracted pending beat");
        ready=1;repeat(5) @(negedge clk);
        if(v || beats!=1 || packets!=0 || state!=5) $fatal(1,"ABORT drain invented success");
        // The same rule applies when a genuine final beat is already pending:
        // it is delivered once, but the session must stay faulted after ABORT.
        reset_case();n=2;limit_n=1;ready=0;start_case();pair_word(0);pair_word(1);
        @(negedge clk);abort_c=1;@(negedge clk);abort_c=0;
        repeat(8) @(negedge clk);
        if(!v || !last || state!=5 || !faults[2]) $fatal(1,"ABORT of stalled TLAST");
        ready=1;repeat(5) @(negedge clk);
        if(v || beats!=1 || packets!=1 || state!=5) $fatal(1,"ABORT lost fault after final handshake");
        // STOP halfway through a packet and then no more radar data: do not
        // fabricate padding or TLAST. Explicit ABORT is the escape mechanism.
        reset_case();n=8;limit_n=0;start_case();pair_word(0);
        @(negedge clk);stop=1;@(negedge clk);stop=0;
        repeat(40) @(negedge clk);
        if(state==4 || v || packets || faults) $fatal(1,"STOP padded unfinished packet");
        abort_c=1;@(negedge clk);abort_c=0;repeat(3) @(negedge clk);
        if(state!=5 || !faults[2] || v) $fatal(1,"half-packet ABORT");
        $display("PASS tb_packetizer: finite/skip/random-stall/overflow/STOP-half/ABORT-held-beat/ABORT-TLAST/odd-length/framing");$finish;
    end
    initial begin #1000000;$fatal(1,"timeout");end
endmodule
