`timescale 1ns/1ps
module tb_decoder;
    reg clk=0; always #6.667 clk=~clk;
    reg resetn=0;
    reg [4:0] cfg=0;
    reg [7:0] f=0,d0=0,d1=0;
    wire [31:0] pair_data,words_seen,errors_seen;
    wire valid,first,err,idle;
    wire [3:0] phase;
    integer got=0,expected_n=0,bitpos=0,total_cases=0;
    reg checking=0,expect_errors=0;
    reg [31:0] expected[0:63];
    reg [7:0] bf=0,b0=0,b1=0;
    reg [15:0] a,b;
    radar_fclk16_decode dut(.clk(clk),.resetn(resetn),.cfg(cfg),.fr_bits(f),.d0_bits(d0),.d1_bits(d1),
        .pair_data(pair_data),.pair_valid(valid),.burst_first(first),.framing_error(err),
        .idle_gap(idle),.phase(phase),.words_seen(words_seen),.errors_seen(errors_seen));
    always @(posedge clk) begin
        #0.01;
        if(checking && !expect_errors) begin
            if(err) $fatal(1,"Unexpected framing error case=%0d",total_cases);
            if(valid) begin
                if(got>=expected_n) $fatal(1,"Unexpected extra raw word");
                if(pair_data!==expected[got]) $fatal(1,"Raw word %0d got %h expected %h",got,pair_data,expected[got]);
                got=got+1;
            end
        end
    end
    task putbit(input bit vf,input bit v0,input bit v1);
        bf[bitpos]=vf^cfg[0]; b0[bitpos]=v0^cfg[1]; b1[bitpos]=v1^cfg[2];
        bitpos=bitpos+1;
        if(bitpos==8) begin
            @(negedge clk); f=bf;d0=b0;d1=b1;bitpos=0;
        end
    endtask
    task quiet(input integer bits);
        repeat(bits) putbit(0,0,0);
    endtask
    task emitword(input reg [15:0] va,input reg [15:0] vb);
        for(integer j=0;j<16;j=j+1)
            putbit(j<8,cfg[3]?va[15-j]:va[j],cfg[3]?vb[15-j]:vb[j]);
    endtask
    initial begin
        for(integer mode=0;mode<32;mode=mode+1) begin
            for(integer p=0;p<8;p=p+1) begin
                checking=0;resetn=0; cfg=mode;bitpos=0;got=0;expected_n=32;
                f={8{cfg[0]}};d0=0;d1=0;
                repeat(4) @(negedge clk);resetn=1;checking=1;
                quiet(40+p);
                for(integer i=0;i<32;i=i+1) begin
                    a=(16'h1357+i*16'h0179);b=(16'h9a31^(i*16'h0365));
                    expected[i]=cfg[4]?{a,b}:{b,a};
                    emitword(a,b);
                    if(i==10 || i==21) quiet(32+(i%7));
                end
                quiet(64);
                while(bitpos!=0) putbit(0,0,0);
                repeat(4) @(negedge clk);
                if(got!=32 || words_seen!=32 || errors_seen!=0) $fatal(1,"Count/phase failure mode=%0d phase=%0d got=%0d",mode,p,got);
                total_cases=total_cases+1;
            end
        end
        checking=0;resetn=0;cfg=0;f=0;bitpos=0;
        repeat(4) @(negedge clk);resetn=1;expect_errors=1;checking=1;
        quiet(40); repeat(7) putbit(1,0,0);quiet(41);
        while(bitpos!=0) putbit(0,0,0);
        repeat(4) @(negedge clk);
        if(errors_seen==0) $fatal(1,"Malformed FRCLK was not detected");
        $display("PASS tb_decoder: 256 polarity/order/phase cases, 8192 words; malformed FRCLK rejected");
        $finish;
    end
    initial begin #10000000;$fatal(1,"timeout");end
endmodule
