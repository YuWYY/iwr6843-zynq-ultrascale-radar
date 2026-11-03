`timescale 1ns/1ps
// Test-only sequential batch driver. The DDR peer matches tb_dsp_engine.
// Fast run uses the official AMD C-model DPI adapter, not native FFT timing.
module tb_dsp_batch;
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
// Follows the unchanged behavioral DDR peer of tb_dsp_engine. This
// test-only layout has sixteen reusable input slots and separate scratch.
 reg [63:0] expected[0:98303],metadata[0:7];
 integer job_number,variant,slots,slot,old_reads,old_writes,trace;
 reg [31:0] input_address;reg [63:0] range_hash,cube_hash,rd_hash,spectrum_hash,state_hash,candidate_hash;
 string references,variant_dir,job_dir;
 task automatic check_region(input string file_name,input [31:0] address,input integer count,input integer skip_cycles,output reg [63:0] digest);
  integer n,dump;reg [63:0] actual;
  begin
   $readmemh(file_name,expected,0,count-1);digest=64'hcbf29ce484222325;
   for(n=0;n<count;n=n+1)begin
    actual=memory[(address-32'h02600000)/8+n];
    digest=(digest^actual)*64'h100000001b3;
    if(!(skip_cycles&&n==3)&&actual!==expected[n])begin
     dump=$fopen($sformatf("%s/failure_job%02d_%08h.hex",outdir,job_number,address),"w");
     for(i=0;i<count;i=i+1)$fdisplay(dump,"%016h",memory[(address-32'h02600000)/8+i]);$fclose(dump);
     dump=$fopen($sformatf("%s/failure_job%02d_input.hex",outdir,job_number),"w");
     for(i=0;i<98304;i=i+1)$fdisplay(dump,"%016h",memory[(input_address-32'h02600000)/8+i]);$fclose(dump);
     $fatal(1,"batch mismatch job=%0d source=%s index=%0d actual=%h expected=%h",job_number,file_name,n,actual,expected[n]);
    end
   end
  end
 endtask
 initial begin
  if(!$value$plusargs("REFERENCE=%s",references))$fatal(1,"batch REFERENCE required");
  if(!$value$plusargs("OUT=%s",outdir))$fatal(1,"batch OUT required");
  for(i=0;i<2097152;i=i+1)memory[i]=0;
  $readmemh({references,"/initial_temporal.hex"},memory,(32'h03400000-32'h02600000)/8,(32'h03400000-32'h02600000)/8+65543);
  cfg[5*32+:32]=2;cfg[9*32+:32]=32'h03200000;cfg[10*32+:32]=32'h032c0000;
  cfg[11*32+:32]=32'h03380000;cfg[12*32+:32]=32'h03390000;cfg[13*32+:32]=32'h03391000;
  cfg[14*32+:32]=32'h03400000;cfg[15*32+:32]=786432;cfg[20*32+:32]=7;
  cfg[23*32+:32]=1038676;cfg[24*32+:32]=262144;cfg[25*32+:32]=131072;
  repeat(16)@(negedge clk);rst_n=1;repeat(8)@(negedge clk);
  trace=$fopen({outdir,"/jobs.jsonl"},"w");
  for(job_number=0;job_number<64;job_number=job_number+1)begin
   if(busy||reading||writing||rv||rsv||wsv)$fatal(1,"slot reuse before prior job transactions settled");
   variant=job_number==63?12:job_number%12;slots=job_number<32?8:16;slot=job_number%slots;
   variant_dir=$sformatf("%s/variant%02d",references,variant);job_dir=$sformatf("%s/job%02d",references,job_number);
   $readmemh({job_dir,"/meta.hex"},metadata,0,7);
   input_address=32'h02600000+slot*32'h000c0000;
   $readmemh({variant_dir,"/input.hex"},memory,(input_address-32'h02600000)/8,(input_address-32'h02600000)/8+98303);
   cfg[6*32+:32]=job_number+1;cfg[7*32+:32]=metadata[5];cfg[8*32+:32]=input_address;
   cfg[16*32+:32]=metadata[4][31:0];cfg[17*32+:32]=metadata[4][63:32];
   cfg[18*32+:32]=metadata[7];cfg[19*32+:32]=metadata[6];cfg[21*32+:32]=metadata[3];
   old_reads=reads;old_writes=writes;
   @(negedge clk);start=1;@(negedge clk);start=0;
   fork
    begin wait(done);end
    begin repeat(8000000)@(posedge clk);$fatal(1,"batch job watchdog job=%0d state=%0d",job_number,dut.state);end
   join_any
   disable fork;
   @(negedge clk);
   if(error||overflows||vectors!=3456||flags!=metadata[0]||candidates!=metadata[1]||reads-old_reads!=14721||writes-old_writes!=11653)
    $fatal(1,"batch result job=%0d error=%h overflow=%0d vectors=%0d flags=%h candidates=%0d reads=%0d writes=%0d",job_number,error,overflows,vectors,flags,candidates,reads-old_reads,writes-old_writes);
   check_region({variant_dir,"/range.hex"},32'h03200000,98304,0,range_hash);
   check_region({variant_dir,"/cube.hex"},32'h032c0000,98304,0,cube_hash);
   check_region({variant_dir,"/rd.hex"},32'h03380000,8192,0,rd_hash);
   check_region({variant_dir,"/spectrum.hex"},32'h03390000,256,0,spectrum_hash);
   check_region({job_dir,"/candidates.hex"},32'h03391000,136,1,candidate_hash);
   check_region({job_dir,"/temporal.hex"},32'h03400000,65544,0,state_hash);
   $fdisplay(trace,"{\"job\":%0d,\"variant\":%0d,\"slots\":%0d,\"slot\":%0d,\"timestamp_us\":%0d,\"epoch\":%0d,\"acquisition\":%0d,\"frame\":%0d,\"cycles_adapter\":%0d,\"reads\":%0d,\"writes\":%0d,\"flags\":%0d,\"candidates\":%0d,\"range_fnv_word64\":\"%016h\",\"cube_fnv_word64\":\"%016h\",\"rd_fnv_word64\":\"%016h\",\"spectrum_fnv_word64\":\"%016h\",\"candidate_fnv_word64\":\"%016h\",\"temporal_fnv_word64\":\"%016h\",\"pass\":true}",job_number+1,variant,slots,slot,metadata[4],metadata[5],metadata[6],metadata[7],cycles,reads-old_reads,writes-old_writes,flags,candidates,range_hash,cube_hash,rd_hash,spectrum_hash,candidate_hash,state_hash);
   $fflush(trace);
   $display("BATCH_JOB_PASS job=%0d slots=%0d slot=%0d variant=%0d cycles_adapter=%0d flags=%h candidates=%0d",job_number+1,slots,slot,variant,cycles,flags,candidates);
   repeat(4)@(negedge clk);
  end
  $fclose(trace);$display("PASS dsp_batch64 official_Cmodel_DPI production_engine_XPM continuous_no_reset");$finish;
 end
endmodule

