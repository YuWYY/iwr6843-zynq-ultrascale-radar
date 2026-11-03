`timescale 1ns/1ps
module tb_dsp_cfar;
 reg clk=0,rst_n=0,start=0,enhanced=0;always #5 clk=~clk;
 wire busy,done,local_valid,local_peak;wire [12:0] address,local_address;
 reg [63:0] raw_data,detector_data;reg gate;reg [63:0] raw[0:8191],detect[0:8191];reg gates[0:8191];
 wire [31:0] count,total,rr,kk,nn;wire [63:0] pp,noise;reg [4:0] idx=0;
 integer i,j,expected_j,nms_count=0,mode=0;
 radar_dsp_cfar u(.clk(clk),.rst_n(rst_n),.start(start),.threshold_q16(32'd1038676),.enhanced(enhanced),
 .busy(busy),.done(done),.address(address),.detector_data(detector_data),.raw_data(raw_data),.fresh_gate(gate),
 .local_valid(local_valid),.local_address(local_address),.local_peak(local_peak),.count(count),.total(total),
 .result_index(idx),.result_range(rr),.result_doppler(kk),.result_power(pp),.result_noise(noise),.result_noise_count(nn));
 always @(posedge clk)begin raw_data<=raw[address];detector_data<=enhanced?detect[address]:raw[address];gate<=gates[address];
  if(local_valid)begin
   nms_count=nms_count+1;
   if(mode==3&&local_address==20*32+6&&local_peak)$fatal(1,"NMS equal neighbor must lose to smaller unsigned Doppler index");
  end end
 initial begin
  for(i=0;i<8192;i=i+1)begin raw[i]=1;detect[i]=64'd4294967296;gates[i]=1;end
  for(j=0;j<33;j=j+1)begin raw[(10+5*j)*32+(j*7)%32]=100000+j;detect[(10+5*j)*32+(j*7)%32]=(64'd1032-j)<<32;end
  gates[(10+5*32)*32+(32*7)%32]=0;
  repeat(8)@(negedge clk);rst_n=1;
  for(mode=0;mode<2;mode=mode+1)begin
   enhanced=mode;nms_count=0;@(negedge clk);start=1;@(negedge clk);start=0;wait(done);@(negedge clk);
   if(count!=32||total!=(mode?32:33)||nms_count!=6368)$fatal(1,"CFAR counters mode=%0d count=%0d total=%0d nms=%0d",mode,count,total,nms_count);
   for(j=0;j<32;j=j+1)begin
    idx=j;#1;expected_j=mode?j:32-j;
    if(rr!=10+5*expected_j||kk!=(expected_j*7)%32||pp!=100000+expected_j||nn==0||noise==0)
     $fatal(1,"CFAR ordering/physical power mode=%0d index=%0d got r=%0d k=%0d p=%0d",mode,j,rr,kk,pp);
   end
   $display("PASS CFAR mode=%0d total=%0d top32/raw_power/ties/window",mode,total);
  end
  for(i=0;i<8192;i=i+1)begin raw[i]=1;detect[i]=64'hffffffffffffffff;gates[i]=1;end
  enhanced=1;nms_count=0;@(negedge clk);start=1;@(negedge clk);start=0;wait(done);@(negedge clk);
  if(count!=0||total!=0||nms_count!=6368)$fatal(1,"72-bit training sum overflow regression");
  $display("PASS CFAR 72-bit full-scale detector training sum: no wrapped false positives");
  for(i=0;i<8192;i=i+1)begin raw[i]=1;gates[i]=1;end
  raw[20*32+5]=1000000;raw[20*32+6]=1000000;raw[25*32+8]=1000000;
  mode=3;enhanced=0;nms_count=0;@(negedge clk);start=1;@(negedge clk);start=0;wait(done);@(negedge clk);
  if(count!=2||total!=2||nms_count!=6368)$fatal(1,"CFAR equal peaks count");
  idx=0;#1;if(rr!=20||kk!=5||pp!=1000000)$fatal(1,"equal score first rank tie");
  idx=1;#1;if(rr!=25||kk!=8||pp!=1000000)$fatal(1,"equal score second rank tie");
  $display("PASS CFAR equal-neighbor NMS and separated equal-score rank ties");
  $finish;
 end
 initial begin #30000000;$fatal(1,"CFAR watchdog");end
endmodule
