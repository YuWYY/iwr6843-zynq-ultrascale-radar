`timescale 1ns/1ps
// CO-SIMULATION ONLY. Arithmetic comes directly from AMD's bit-accurate DLL.
// The chosen-delay AXIS adapter below is not the vendor HDL protocol
// model and is never used as hardware-cycle or native-FFT verification proof.
module radar_fft_cmodel_axis #(parameter N=256, CW=24, DELAY_CYCLES=1024)(
 input wire aclk,aresetn,input wire [CW-1:0] s_axis_config_tdata,
 input wire s_axis_config_tvalid,output wire s_axis_config_tready,
 input wire [47:0] s_axis_data_tdata,input wire s_axis_data_tvalid,
 output wire s_axis_data_tready,input wire s_axis_data_tlast,
 output reg [47:0] m_axis_data_tdata,output reg [7:0] m_axis_data_tuser,
 output reg m_axis_data_tvalid,input wire m_axis_data_tready,output reg m_axis_data_tlast,
 output reg [7:0] m_axis_status_tdata,output reg m_axis_status_tvalid,input wire m_axis_status_tready,
 output reg event_frame_started,event_tlast_unexpected,event_tlast_missing,event_fft_overflow,
 output wire event_status_channel_halt,event_data_in_channel_halt,event_data_out_channel_halt
);
 import "DPI-C" function void radar_vendor_fft_put(input int n,input int index,input int re,input int im);
 import "DPI-C" function int radar_vendor_fft_run(input int n,input int schedule,input int direction);
 import "DPI-C" function longint unsigned radar_vendor_fft_get(input int n,input int index);
 integer input_index,output_index,wait_cycles,phase,rc;reg configured;reg [CW-1:0] configuration;
 assign s_axis_config_tready=phase==0&&input_index==0;
 assign s_axis_data_tready=configured&&phase==0;
 assign event_status_channel_halt=m_axis_status_tvalid&&!m_axis_status_tready;
 assign event_data_out_channel_halt=m_axis_data_tvalid&&!m_axis_data_tready;
 assign event_data_in_channel_halt=s_axis_data_tvalid&&!s_axis_data_tready;
 always @(posedge aclk)begin
  if(!aresetn)begin
   phase<=0;configured<=0;configuration<=0;input_index<=0;output_index<=0;wait_cycles<=0;
   m_axis_data_tdata<=0;m_axis_data_tuser<=0;m_axis_data_tvalid<=0;m_axis_data_tlast<=0;
   m_axis_status_tdata<=0;m_axis_status_tvalid<=0;event_frame_started<=0;
   event_tlast_unexpected<=0;event_tlast_missing<=0;event_fft_overflow<=0;
  end else begin
   event_frame_started<=0;event_tlast_unexpected<=0;event_tlast_missing<=0;event_fft_overflow<=0;
   if(m_axis_status_tvalid&&m_axis_status_tready)m_axis_status_tvalid<=0;
   if(s_axis_config_tvalid&&s_axis_config_tready)begin configuration<=s_axis_config_tdata;configured<=1;end
   if(s_axis_data_tvalid&&s_axis_data_tready)begin
    if(input_index==0)event_frame_started<=1;
    if(s_axis_data_tlast!=(input_index==N-1))begin
     event_tlast_unexpected<=s_axis_data_tlast;event_tlast_missing<=!s_axis_data_tlast;
    end
    radar_vendor_fft_put(N,input_index,$signed(s_axis_data_tdata[23:0]),$signed(s_axis_data_tdata[47:24]));
    if(input_index==N-1)begin
     rc=radar_vendor_fft_run(N,configuration>>1,configuration[0]);
     if(rc<0)$fatal(1,"official AMD FFT C-model failed rc=%0d",rc);
     m_axis_status_tdata<=rc;event_fft_overflow<=rc!=0;
     wait_cycles<=DELAY_CYCLES;phase<=1;input_index<=0;
    end else input_index<=input_index+1;
   end
   if(phase==1)begin
    if(wait_cycles==0)begin
     m_axis_data_tdata<=radar_vendor_fft_get(N,0);m_axis_data_tuser<=0;m_axis_data_tlast<=0;
     m_axis_data_tvalid<=1;output_index<=0;phase<=2;
    end else wait_cycles<=wait_cycles-1;
   end
   if(m_axis_data_tvalid&&m_axis_data_tready)begin
    if(output_index==N-1)begin m_axis_data_tvalid<=0;m_axis_status_tvalid<=1;phase<=0;end
    else begin
     output_index<=output_index+1;m_axis_data_tdata<=radar_vendor_fft_get(N,output_index+1);
     m_axis_data_tuser<=output_index+1;m_axis_data_tlast<=output_index==N-2;
    end
   end
  end
 end
endmodule

`define FFT_PORTS(CW) input wire aclk,aresetn,input wire [CW-1:0] s_axis_config_tdata,input wire s_axis_config_tvalid,output wire s_axis_config_tready,input wire [47:0] s_axis_data_tdata,input wire s_axis_data_tvalid,output wire s_axis_data_tready,input wire s_axis_data_tlast,output wire [47:0] m_axis_data_tdata,output wire [7:0] m_axis_data_tuser,output wire m_axis_data_tvalid,input wire m_axis_data_tready,output wire m_axis_data_tlast,output wire [7:0] m_axis_status_tdata,output wire m_axis_status_tvalid,input wire m_axis_status_tready,output wire event_frame_started,event_tlast_unexpected,event_tlast_missing,event_fft_overflow,event_status_channel_halt,event_data_in_channel_halt,event_data_out_channel_halt
module radar_fft_range(`FFT_PORTS(24));radar_fft_cmodel_axis #(256,24,1024) model(.*);endmodule
module radar_fft_doppler(`FFT_PORTS(16));radar_fft_cmodel_axis #(32,16,256) model(.*);endmodule
`undef FFT_PORTS
