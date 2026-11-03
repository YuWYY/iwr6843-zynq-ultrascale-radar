`timescale 1ns/1ps
module fft_probe #(parameter N=256)(output reg finished=0);
 reg clk=0,rst_n=0,cv=0,iv=0,il=0;reg [47:0] idata=0;
 wire cr,ir,ov,ol,sv,overflow;wire [47:0] odata;wire [7:0] user,status;
 integer test_id=0,received=0,j,re,im,expected;real phase;
 always #5 clk=~clk;
 generate if(N==256)begin
 radar_fft_range u(.aclk(clk),.aresetn(rst_n),.s_axis_config_tdata(24'h00aaab),.s_axis_config_tvalid(cv),.s_axis_config_tready(cr),
 .s_axis_data_tdata(idata),.s_axis_data_tvalid(iv),.s_axis_data_tready(ir),.s_axis_data_tlast(il),
 .m_axis_data_tdata(odata),.m_axis_data_tuser(user),.m_axis_data_tvalid(ov),.m_axis_data_tready(1'b1),.m_axis_data_tlast(ol),
 .m_axis_status_tdata(status),.m_axis_status_tvalid(sv),.m_axis_status_tready(1'b1),.event_fft_overflow(overflow));
 end else begin
 radar_fft_doppler u(.aclk(clk),.aresetn(rst_n),.s_axis_config_tdata(16'h02ab),.s_axis_config_tvalid(cv),.s_axis_config_tready(cr),
 .s_axis_data_tdata(idata),.s_axis_data_tvalid(iv),.s_axis_data_tready(ir),.s_axis_data_tlast(il),
 .m_axis_data_tdata(odata),.m_axis_data_tuser(user),.m_axis_data_tvalid(ov),.m_axis_data_tready(1'b1),.m_axis_data_tlast(ol),
 .m_axis_status_tdata(status),.m_axis_status_tvalid(sv),.m_axis_status_tready(1'b1),.event_fft_overflow(overflow));
 end endgenerate
 always @(posedge clk)if(rst_n)begin
  if(overflow||(sv&&status!=0))$fatal(1,"FFT overflow/status N=%0d",N);
  if(ov)begin
   re=$signed(odata[23:0]);im=$signed(odata[47:24]);
   expected=test_id==0?1048576/N:((received==(test_id==1?0:5))?1048576:0);
   if(re<expected-64||re>expected+64||im< -64||im>64)$fatal(1,"FFT mismatch N=%0d case=%0d bin=%0d re=%0d im=%0d expected=%0d",N,test_id,received,re,im,expected);
   if(ol!=(received==N-1))$fatal(1,"FFT TLAST N=%0d bin=%0d",N,received);
   received=received+1;
  end
 end
 initial begin
  repeat(12)@(negedge clk);rst_n=1;repeat(4)@(negedge clk);cv=1;
  do @(posedge clk);while(!cr);@(negedge clk);cv=0;
  for(test_id=0;test_id<3;test_id=test_id+1)begin
   received=0;
   for(j=0;j<N;j=j+1)begin
    if(test_id==0)begin idata=0;if(j==0)idata[23:0]=1048576;end
    else if(test_id==1)idata={24'd0,24'd1048576};
    else begin phase=2.0*3.14159265358979323846*5*j/N;
      idata[23:0]=$rtoi(1048576.0*$cos(phase));idata[47:24]=$rtoi(1048576.0*$sin(phase));end
    iv=1;il=j==N-1;do @(posedge clk);while(!ir);@(negedge clk);
   end
   iv=0;il=0;wait(received==N);repeat(8)@(negedge clk);
   $display("FFT_PROBE_PASS N=%0d test=%0d forward natural_order scale=1/N",N,test_id);
  end
  finished=1;
 end
endmodule
module tb_fft_scale;
 wire a,b;fft_probe #(256)r(a);fft_probe #(32)d(b);
 initial begin wait(a&&b);$display("PASS native_fft_scale");$finish;end
 initial begin #1000000;$fatal(1,"FFT watchdog");end
endmodule
