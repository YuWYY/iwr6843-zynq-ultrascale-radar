`timescale 1ns/1ps
// Behavioral DDR/DataMover protocol peer, NOT a substitute FFT. Both native
// generated FFT simulation models are instantiated inside the production DUT.
module tb_dsp_engine;
 reg clk=0,rst_n=0,start=0,cancel=0;reg [1023:0] cfg=0;
 wire busy,done;wire [31:0] error,candidates,overflows,vectors,flags;wire [63:0] cycles;
 wire [71:0] rcmd,wcmd;wire rcv,wcv;reg rcr=0,wcr=0;
 reg [7:0] rs=0,ws=0;reg rsv=0,wsv=0;wire rsr,wsr;
 reg [63:0] rdata=0;reg rv=0,rl=0;wire rr;wire [63:0] wdata;wire wv,wl;reg wr=0;
 reg [63:0] memory[0:2097151];
 reg reading=0,writing=0;reg [31:0] ra=0,wa=0,rleft=0,wleft=0;
 reg [31:0] random_state=32'h68a31234;integer i,f,reads=0,writes=0,frames=0;
 string fixture,outdir,temporal_fixture;integer mode=1,test_case=0,ticks=0,halt_ticks=0;
 wire rh,wh;reg rhc=0,whc=0;
 always #5 clk=~clk;
 radar_dsp_engine dut(.clk(clk),.rst_n(rst_n),.start(start),.cancel(cancel),.cfg(cfg),
  .mm2s_halt(rh),.s2mm_halt(wh),.mm2s_halt_cmplt(rhc),.s2mm_halt_cmplt(whc),
  .busy(busy),.done(done),.error(error),.cycles(cycles),.candidate_count(candidates),.overflow_count(overflows),.vector_count(vectors),.result_flags(flags),
  .mm2s_cmd_tdata(rcmd),.mm2s_cmd_tvalid(rcv),.mm2s_cmd_tready(rcr),.mm2s_sts_tdata(rs),.mm2s_sts_tvalid(rsv),.mm2s_sts_tready(rsr),
  .mm2s_tdata(rdata),.mm2s_tvalid(rv),.mm2s_tready(rr),.mm2s_tlast(rl),
  .s2mm_cmd_tdata(wcmd),.s2mm_cmd_tvalid(wcv),.s2mm_cmd_tready(wcr),.s2mm_sts_tdata(ws),.s2mm_sts_tvalid(wsv),.s2mm_sts_tready(wsr),
  .s2mm_tdata(wdata),.s2mm_tvalid(wv),.s2mm_tready(wr),.s2mm_tlast(wl));
 always @(posedge clk)begin
  if(!rst_n)begin reading<=0;writing<=0;rsv<=0;wsv<=0;rv<=0;rcr<=0;wcr<=0;wr<=0;rhc<=0;whc<=0;halt_ticks=0;end
  else begin
   random_state<={random_state[30:0],random_state[31]^random_state[21]^random_state[1]^random_state[0]};
   rcr<=!rh&&!reading&&!rsv&&random_state[0];wcr<=!wh&&!writing&&!wsv&&random_state[1];wr<=!wh&&writing&&random_state[2];
   if(rh||wh)begin
    halt_ticks=halt_ticks+1;
    if(halt_ticks>=4)begin reading<=0;rv<=0;rhc<=1;end
    if(halt_ticks>=7)begin writing<=0;wr<=0;whc<=1;end
   end
   if(rsv&&rsr)rsv<=0;if(wsv&&wsr)wsv<=0;
   if(rcv&&rcr)begin
    if(reading||rsv||rcmd[63:32]<32'h02600000||rcmd[63:32]+rcmd[22:0]>32'h03600000||rcmd[37:32]!=0||rcmd[5:0]!=0)$fatal(1,"bad read command %h",rcmd);
    reading<=1;ra<=rcmd[63:32];rleft<=rcmd[22:0];rcr<=0;reads=reads+1;
   end
   if(wcv&&wcr)begin
    if(writing||wsv||wcmd[63:32]<32'h02600000||wcmd[63:32]+wcmd[22:0]>32'h03600000||wcmd[37:32]!=0||wcmd[5:0]!=0)$fatal(1,"bad write command %h",wcmd);
    writing<=1;wa<=wcmd[63:32];wleft<=wcmd[22:0];wcr<=0;writes=writes+1;
   end
   if(reading&&(!rv||rr))begin
    if(rv&&rr)begin
     if(rleft==8)begin reading<=0;rv<=0;rsv<=1;rs<=test_case==3?8'h90:8'h80;end
     else begin ra<=ra+8;rleft<=rleft-8;rv<=0;end
    end else if(random_state[3])begin rdata<=memory[(ra-32'h02600000)>>3];rv<=1;rl<=(rleft==8)||(test_case==6&&rleft==2040);end
   end
   if(wv&&wr)begin
    if(!writing||wl!=(wleft==8))$fatal(1,"bad write TLAST");
    memory[(wa-32'h02600000)>>3]<=wdata;
    if(wleft==8)begin writing<=0;wr<=0;wsv<=1;ws<=test_case==4?8'h90:8'h80;end
    else begin wa<=wa+8;wleft<=wleft-8;end
   end
  end
 end
 initial begin
  if(!$value$plusargs("FIXTURE=%s",fixture))fixture="/software/verification/20261101_pl_dsp_01/checks/dsp_engine/fixture/input.hex";
  if(!$value$plusargs("OUT=%s",outdir))outdir="/software/verification/20261101_pl_dsp_01/checks/dsp_engine/fixture";
  if($value$plusargs("MODE=%d",mode))begin end
  if($value$plusargs("CASE=%d",test_case))begin end
  for(i=0;i<2097152;i=i+1)memory[i]=0;
  $readmemh(fixture,memory,0,98303);
  cfg[5*32+:32]=mode;cfg[6*32+:32]=1;cfg[7*32+:32]=1;
  cfg[8*32+:32]=32'h02600000;cfg[9*32+:32]=32'h02700000;cfg[10*32+:32]=32'h02800000;
  cfg[11*32+:32]=32'h02900000;cfg[12*32+:32]=32'h02910000;cfg[13*32+:32]=32'h02911000;
  cfg[14*32+:32]=32'h02a00000;cfg[15*32+:32]=786432;
  cfg[16*32+:32]=100000;cfg[18*32+:32]=1;cfg[19*32+:32]=1;cfg[20*32+:32]=7;
  cfg[21*32+:32]=104700000;cfg[23*32+:32]=1038676;cfg[24*32+:32]=262144;cfg[25*32+:32]=131072;
  if(test_case==5)cfg[10*32+:32]=32'h02400000;
  if(test_case==7)begin cfg[5*32+:32]=2;mode=2;cfg[20*32+:32]=3;end
  if(test_case==8||test_case==9)begin
   cfg[5*32+:32]=2;mode=2;cfg[16*32+:32]=1000000;cfg[18*32+:32]=10;
   if(!$value$plusargs("TEMPORAL=%s",temporal_fixture))$fatal(1,"CASE8/9 requires temporal state fixture");
   $readmemh(temporal_fixture,memory,(32'h02a00000-32'h02600000)/8,(32'h02a00000-32'h02600000)/8+65543);
  end
  repeat(16)@(negedge clk);rst_n=1;repeat(8)@(negedge clk);start=1;@(negedge clk);start=0;
  wait(done);@(negedge clk);
  if(test_case!=0&&test_case!=7&&test_case!=8)begin
   if(test_case==1||test_case==2)begin if(error!=4)$fatal(1,"cancel result %h",error);end
   else if(test_case==5)begin if(error!=1||reads!=0||writes!=0)$fatal(1,"invalid address result");end
   else if(test_case==9)begin
    if(error!=32'h303||overflows!=1||candidates!=0||writes!=2)$fatal(1,"temporal numerical overflow must reject frame without commit err=%h ov=%0d cnt=%0d wr=%0d",error,overflows,candidates,writes);
    for(i=0;i<8;i=i+1)if(memory[(32'h02a00000-32'h02600000)/8+i]!=0)$fatal(1,"overflow left temporal header valid");
   end
   else if(!error[31]||!rhc||!whc||busy)$fatal(1,"terminal locked error before both halt completions");
   $display("PASS dsp_engine CASE=%0d error=%h cycles=%0d reads=%0d writes=%0d",test_case,error,cycles,reads,writes);$finish;
  end
  if(error!=0||overflows!=0||vectors!=3456)$fatal(1,"ENGINE FAIL error=%h overflows=%0d vectors=%0d state=%0d",error,overflows,vectors,dut.state);
  if(test_case==7)begin
   if(flags!=32'h4c0||candidates==0)$fatal(1,"TIME_UNTRUSTED must produce baseline candidates flags=%h count=%0d",flags,candidates);
   for(i=0;i<65544;i=i+1)if(memory[(32'h02a00000-32'h02600000)/8+i]!=0)$fatal(1,"untrusted state/header not cleared word=%0d",i);
  end
  if(test_case==8&&flags!=0)$fatal(1,"trusted initialized enhancement flags=%h",flags);
  f=$fopen({outdir,"/rtl_range.hex"},"w");for(i=0;i<98304;i=i+1)$fdisplay(f,"%016h",memory[(32'h02700000-32'h02600000)/8+i]);$fclose(f);
  f=$fopen({outdir,"/rtl_cube.hex"},"w");for(i=0;i<98304;i=i+1)$fdisplay(f,"%016h",memory[(32'h02800000-32'h02600000)/8+i]);$fclose(f);
  f=$fopen({outdir,"/rtl_spectrum.hex"},"w");for(i=0;i<256;i=i+1)$fdisplay(f,"%016h",memory[(32'h02910000-32'h02600000)/8+i]);$fclose(f);
  f=$fopen({outdir,"/rtl_rd.hex"},"w");for(i=0;i<8192;i=i+1)$fdisplay(f,"%016h",memory[(32'h02900000-32'h02600000)/8+i]);$fclose(f);
  f=$fopen({outdir,"/rtl_candidates.hex"},"w");for(i=0;i<136;i=i+1)$fdisplay(f,"%016h",memory[(32'h02911000-32'h02600000)/8+i]);$fclose(f);
  if(mode==2)begin f=$fopen({outdir,"/rtl_temporal.hex"},"w");for(i=0;i<65544;i=i+1)$fdisplay(f,"%016h",memory[(32'h02a00000-32'h02600000)/8+i]);$fclose(f);end
  $display("PASS dsp_engine mode=%0d cycles=%0d vectors=%0d candidates=%0d reads=%0d writes=%0d flags=%h",mode,cycles,vectors,candidates,reads,writes,flags);$finish;
 end
 always @(posedge clk)if(rst_n)begin
  ticks=ticks+1;
  if(ticks%100000==0)$display("ENGINE_PROGRESS cycle=%0d state=%0d vec=%0d tile=%0d range=%0d channel=%0d",ticks,dut.state,dut.vector_index,dut.tile_base,dut.range_in_tile,dut.channel_index);
 end
 initial begin
  wait(rst_n);
  if(test_case==1)begin wait(reading&&rv);@(negedge clk);cancel=1;@(negedge clk);cancel=0;end
  if(test_case==2)begin wait(writing&&wv);@(negedge clk);cancel=1;@(negedge clk);cancel=0;end
  if(test_case==9)begin
   // Focused controller fault injection: seed the post-RD checkpoint directly.
   // This case tests real temporal arithmetic/memory/abort only, never FFT proof.
   wait(dut.state==2);@(negedge clk);
   force dut.rd_read_data=64'd100000000000000;dut.local_mask[0]=1;
   memory[(32'h02a00000-32'h02600000)/8+8]=1;
   dut.state=25;
   wait(done);release dut.rd_read_data;
  end
 end
 initial begin #80000000;$fatal(1,"engine watchdog state=%0d vec=%0d tile=%0d range=%0d chan=%0d memr=%b memw=%b",dut.state,dut.vector_index,dut.tile_base,dut.range_in_tile,dut.channel_index,dut.rd_busy,dut.wr_busy);end
endmodule
