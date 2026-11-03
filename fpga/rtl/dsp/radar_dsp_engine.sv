`timescale 1ns/1ps
// DDR-backed fixed-geometry DSP. Native XFFT cores are the only FFT engines.
// All memory commands are drained before completion, including cancellation.
module radar_dsp_engine(
 input wire clk,rst_n,start,cancel,input wire [1023:0] cfg,
 output wire mm2s_halt,s2mm_halt,input wire mm2s_halt_cmplt,s2mm_halt_cmplt,
 output reg busy,done,output reg [31:0] error,output reg [63:0] cycles,
 output reg [31:0] candidate_count,overflow_count,vector_count,result_flags,
 output wire [71:0] mm2s_cmd_tdata,output wire mm2s_cmd_tvalid,input wire mm2s_cmd_tready,
 input wire [7:0] mm2s_sts_tdata,input wire mm2s_sts_tvalid,output wire mm2s_sts_tready,
 input wire [63:0] mm2s_tdata,input wire mm2s_tvalid,output wire mm2s_tready,input wire mm2s_tlast,
 output wire [71:0] s2mm_cmd_tdata,output wire s2mm_cmd_tvalid,input wire s2mm_cmd_tready,
 input wire [7:0] s2mm_sts_tdata,input wire s2mm_sts_tvalid,output wire s2mm_sts_tready,
 output wire [63:0] s2mm_tdata,output wire s2mm_tvalid,input wire s2mm_tready,output wire s2mm_tlast
);
 localparam IDLE=0,CHECK=1,CLEAR=2,CONFIG=3,R_READ=4,R_COLLECT=5,R_MEAN=6,
  PRE_LOAD=7,PRE_MUL=8,PRE_STORE=9,R_WRITE=10,F_LOAD=11,F_PUSH=12,F_WAIT=13,
  TILE_START=14,TILE_READ=15,TILE_COLLECT=16,D_PREP=17,D_WRITE=18,
  OUTPUT_START=19,OUTPUT_LOAD=20,OUTPUT_PUSH=21,OUTPUT_WAIT=22,
  CFAR_START=23,CFAR_WAIT=24,T_HEADER_READ=25,T_HEADER_GET=26,T_BEGIN=27,T_BEGIN_WAIT=28,
  T_ZERO_START=29,T_ZERO_WAIT=30,T_CELL_READ=31,T_CELL_GET=32,T_CELL_SEND=33,
  T_CELL_RESULT=34,T_CELL_WRITE=35,T_CELL_WAIT=36,T_COMMIT=37,T_COMMIT_WAIT=38,
  FINISH=39,FAULT_DRAIN=40,OUTPUT_FETCH=41,PRE_FETCH=42;
 reg [5:0] state;reg [1023:0] job;reg cancel_pending;
 reg [13:0] clear_index;reg [8:0] vector_index;reg [8:0] sample_index,prep_index,send_index,out_index;
 reg [7:0] tile_base;reg tile_bank;reg [3:0] range_in_tile,channel_index;
 reg [12:0] cell_index;reg [3:0] cell_word;
 reg [31:0] input_base,range_base,cube_base,rd_base,spectrum_base,candidates_base,temporal_base;
 reg signed [23:0] vector_re[0:255],vector_im[0:255];
 reg local_mask[0:8191],fresh_mask[0:8191];
 reg signed [39:0] sum_re,sum_im;reg signed [23:0] mean_re,mean_im;
 reg signed [24:0] pre_re,pre_im;reg [24:0] coefficient;
 reg signed [50:0] product_re,product_im;reg doppler_pass;
 reg [47:0] fft_input;reg fft_input_valid;wire fft_input_ready;
 reg range_configured,doppler_configured,fft_status_seen;
 wire rcfg_ready,dcfg_ready,rin_ready,din_ready,rout_valid,dout_valid,rout_last,dout_last;
 wire [47:0] rout,dout;wire [7:0] ruser,duser,rstatus,dstatus;
 wire rstatus_valid,dstatus_valid,rovf,dovf,rtunexpected,rtmissing,dtunexpected,dtmissing;
 wire [24:0] window_range,window_doppler;
 wire [47:0] fft_output=doppler_pass?dout:rout;
 wire fft_output_valid=doppler_pass?dout_valid:rout_valid;
 wire fft_output_last=doppler_pass?dout_last:rout_last;
 wire feeding=(state==F_LOAD||state==F_PUSH||state==F_WAIT);
 wire fft_consumed=feeding&&fft_output_valid&&wr_ready;
 assign fft_input_ready=doppler_pass?din_ready:rin_ready;
 radar_dsp_window u_windows(.index(prep_index[7:0]),.range_coefficient(window_range),.doppler_coefficient(window_doppler));
 radar_fft_range u_range_fft(.aclk(clk),.aresetn(rst_n),
  .s_axis_config_tdata(24'h00aaab),.s_axis_config_tvalid(state==CONFIG&&!range_configured),.s_axis_config_tready(rcfg_ready),
  .s_axis_data_tdata(fft_input),.s_axis_data_tvalid(fft_input_valid&&!doppler_pass),.s_axis_data_tready(rin_ready),
  .s_axis_data_tlast(send_index==255),.m_axis_data_tdata(rout),.m_axis_data_tuser(ruser),
  .m_axis_data_tvalid(rout_valid),.m_axis_data_tready(feeding&&!doppler_pass&&wr_ready),.m_axis_data_tlast(rout_last),
  .m_axis_status_tdata(rstatus),.m_axis_status_tvalid(rstatus_valid),.m_axis_status_tready(1'b1),
  .event_frame_started(),.event_tlast_unexpected(rtunexpected),.event_tlast_missing(rtmissing),
  .event_fft_overflow(rovf),.event_status_channel_halt(),.event_data_in_channel_halt(),.event_data_out_channel_halt());
 radar_fft_doppler u_doppler_fft(.aclk(clk),.aresetn(rst_n),
  .s_axis_config_tdata(16'h02ab),.s_axis_config_tvalid(state==CONFIG&&!doppler_configured),.s_axis_config_tready(dcfg_ready),
  .s_axis_data_tdata(fft_input),.s_axis_data_tvalid(fft_input_valid&&doppler_pass),.s_axis_data_tready(din_ready),
  .s_axis_data_tlast(send_index==31),.m_axis_data_tdata(dout),.m_axis_data_tuser(duser),
  .m_axis_data_tvalid(dout_valid),.m_axis_data_tready(feeding&&doppler_pass&&wr_ready),.m_axis_data_tlast(dout_last),
  .m_axis_status_tdata(dstatus),.m_axis_status_tvalid(dstatus_valid),.m_axis_status_tready(1'b1),
  .event_frame_started(),.event_tlast_unexpected(dtunexpected),.event_tlast_missing(dtmissing),
  .event_fft_overflow(dovf),.event_status_channel_halt(),.event_data_in_channel_halt(),.event_data_out_channel_halt());

 reg rd_start,wr_start;reg [31:0] rd_address,wr_address;reg [22:0] rd_bytes,wr_bytes;
 wire rd_busy,rd_done,wr_busy,wr_done,memory_locked,memory_quiescent;wire [31:0] rd_error,wr_error,memory_fault;
 wire [63:0] read_data;wire read_valid,wr_ready;reg read_ready,write_valid;
 reg [63:0] write_data,write_latch;
 radar_dsp_mem u_memory(.clk(clk),.rst_n(rst_n),.fault_locked(memory_locked),.fault_quiescent(memory_quiescent),.fault_code(memory_fault),
  .mm2s_halt(mm2s_halt),.s2mm_halt(s2mm_halt),.mm2s_halt_cmplt(mm2s_halt_cmplt),.s2mm_halt_cmplt(s2mm_halt_cmplt),
  .rd_start(rd_start),.rd_addr(rd_address),.rd_bytes(rd_bytes),
  .rd_busy(rd_busy),.rd_done(rd_done),.rd_error(rd_error),.rd_data(read_data),.rd_valid(read_valid),.rd_ready(read_ready),
  .wr_start(wr_start),.wr_addr(wr_address),.wr_bytes(wr_bytes),.wr_busy(wr_busy),.wr_done(wr_done),.wr_error(wr_error),
  .wr_data(write_data),.wr_valid(write_valid),.wr_ready(wr_ready),
  .mm2s_cmd_tdata(mm2s_cmd_tdata),.mm2s_cmd_tvalid(mm2s_cmd_tvalid),.mm2s_cmd_tready(mm2s_cmd_tready),
  .mm2s_sts_tdata(mm2s_sts_tdata),.mm2s_sts_tvalid(mm2s_sts_tvalid),.mm2s_sts_tready(mm2s_sts_tready),
  .mm2s_tdata(mm2s_tdata),.mm2s_tvalid(mm2s_tvalid),.mm2s_tready(mm2s_tready),.mm2s_tlast(mm2s_tlast),
  .s2mm_cmd_tdata(s2mm_cmd_tdata),.s2mm_cmd_tvalid(s2mm_cmd_tvalid),.s2mm_cmd_tready(s2mm_cmd_tready),
  .s2mm_sts_tdata(s2mm_sts_tdata),.s2mm_sts_tvalid(s2mm_sts_tvalid),.s2mm_sts_tready(s2mm_sts_tready),
  .s2mm_tdata(s2mm_tdata),.s2mm_tvalid(s2mm_tvalid),.s2mm_tready(s2mm_tready),.s2mm_tlast(s2mm_tlast));

 // Round signed values using an arithmetic-floor quotient: remainder is
 // nonnegative, including for negative values. Ties go to an even quotient.
 function automatic signed [63:0] rne(input signed [63:0] x,input integer shift);
  reg signed [63:0] q,rem,half;
  begin q=x>>>shift;rem=x-(q<<<shift);half=64'sd1<<<(shift-1);
   rne=q+((rem>half||(rem==half&&q[0]))?64'sd1:64'sd0);end
 endfunction
 function automatic [23:0] saturate24(input signed [63:0] x);
  begin if(x>8388607)saturate24=24'h7fffff;else if(x< -8388608)saturate24=24'h800000;else saturate24=x[23:0];end
 endfunction
 function automatic [63:0] magnitude(input [47:0] x);
  reg signed [23:0] a,b;reg signed [47:0] aa,bb;
  begin a=x[23:0];b=x[47:24];aa=a*a;bb=b*b;magnitude={16'd0,aa}+{16'd0,bb};end
 endfunction
 function automatic [63:0] ddr_word(input [47:0] x);
  begin ddr_word={{8{x[47]}},x[47:24],{8{x[23]}},x[23:0]};end
 endfunction
 function automatic valid_region(input [31:0] addr,input [31:0] length);
  reg [32:0] finish;
  begin finish={1'b0,addr}+length;valid_region=addr>=32'h02600000&&addr[5:0]==0&&finish<=33'h003600000;end
 endfunction
 function automatic [3:0] physical_channel(input [8:0] vector_number);
  integer chirp,slot,tx;
  begin chirp=vector_number/4;slot=chirp%3;tx=slot==1?2:(slot==2?1:0);
   physical_channel=tx*4+(vector_number%4);end
 endfunction
 integer address_i,address_j;reg address_ok;reg [31:0] bases[0:6],lengths[0:6];
 always @* begin
  for(address_i=0;address_i<7;address_i=address_i+1) bases[address_i]=job[(8+address_i)*32+:32];
  lengths[0]=786432;lengths[1]=786432;lengths[2]=786432;lengths[3]=65536;
  lengths[4]=2048;lengths[5]=1088;lengths[6]=524352;
  address_ok=(job[15*32+:32]==786432)&&(job[5*32+:32]==1||job[5*32+:32]==2);
  for(address_i=0;address_i<7;address_i=address_i+1) begin
   if(address_i<6||job[5*32+:32]==2)begin
    if(!valid_region(bases[address_i],lengths[address_i]))address_ok=0;
    for(address_j=0;address_j<address_i;address_j=address_j+1)
     if(bases[address_i]<bases[address_j]+lengths[address_j]&&bases[address_j]<bases[address_i]+lengths[address_i])address_ok=0;
   end
  end
 end

 // Independent BRAM read and write ports. The one-cycle accumulator pipeline
 // accesses a different FFT bin on consecutive transfers (no same-bin hazard).
 reg power_pending,power_is_doppler;reg [12:0] power_index;
 reg [63:0] power_value;
 wire [63:0] rd_read_data,spectrum_read_data,enhanced_read_data;
 wire [63:0] power_old=power_is_doppler?rd_read_data:spectrum_read_data;
 wire [12:0] doppler_power_address=({5'd0,tile_base}+range_in_tile)*32+out_index;
 wire [12:0] cfar_address;wire [63:0] cfar_raw=rd_read_data;
 wire [63:0] cfar_detector=cfar_enhanced?enhanced_read_data:rd_read_data;reg cfar_gate;
 reg cfar_start,cfar_enhanced;wire cfar_busy,cfar_done;
 wire local_valid,local_peak;wire [12:0] local_address;
 wire [31:0] cfar_count,cfar_total,point_range,point_doppler,point_noise_count;
 wire [63:0] point_power,point_noise;reg [4:0] point_index;
 radar_dsp_cfar u_cfar(.clk(clk),.rst_n(rst_n),.start(cfar_start),
  .threshold_q16(cfar_enhanced?job[24*32+:32]:job[23*32+:32]),.enhanced(cfar_enhanced),
  .busy(cfar_busy),.done(cfar_done),.address(cfar_address),.detector_data(cfar_detector),.raw_data(cfar_raw),.fresh_gate(cfar_gate),
  .local_valid(local_valid),.local_address(local_address),.local_peak(local_peak),.count(cfar_count),.total(cfar_total),
  .result_index(point_index),.result_range(point_range),.result_doppler(point_doppler),
  .result_power(point_power),.result_noise(point_noise),.result_noise_count(point_noise_count));
 // One explicitly muxed synchronous read port and one write port per BRAM.
 // Output reads have a FETCH cycle; FFT accumulation still writes on the
 // cycle immediately after the selected read, preserving the bin pipeline.
 reg [12:0] rd_read_address;
 reg [7:0] spectrum_read_address;
 always @* begin
  rd_read_address=cfar_address;spectrum_read_address=out_index[7:0];
  if(fft_consumed&&doppler_pass)rd_read_address=doppler_power_address;
  else if(state==OUTPUT_FETCH||state==OUTPUT_LOAD)rd_read_address=output_index[12:0];
  else if(state==T_CELL_READ||state==T_CELL_GET)rd_read_address=cell_index;
  if(state==OUTPUT_FETCH||state==OUTPUT_LOAD)spectrum_read_address=output_index[7:0];
 end
 wire rd_we=(state==CLEAR)||(power_pending&&power_is_doppler);
 wire [12:0] rd_wa=state==CLEAR?clear_index[12:0]:power_index;
 wire [63:0] power_wdata=state==CLEAR?64'd0:power_old+power_value;
 wire spectrum_we=(state==CLEAR&&clear_index<256)||(power_pending&&!power_is_doppler);
 wire [7:0] spectrum_wa=state==CLEAR?clear_index[7:0]:power_index[7:0];
 wire enhanced_we=(state==CLEAR)||(temporal_result_valid&&temporal_result_ready);
 wire [12:0] enhanced_wa=state==CLEAR?clear_index[12:0]:temporal_result_index;
 wire [63:0] enhanced_wdata=state==CLEAR?64'd0:temporal_power;
 wire tile_we=state==TILE_COLLECT&&read_valid;
 wire [13:0] tile_wa=tile_bank*6144+(physical_channel(vector_index)*32+vector_index/12)*16+sample_index;
 wire [13:0] tile_ra=tile_bank*6144+(channel_index*32+prep_index)*16+range_in_tile;
 wire [47:0] tile_rdata;
 radar_dsp_bram #(.WIDTH(64),.DEPTH(8192),.ADDR_WIDTH(13)) u_rd_bram(
  .clk(clk),.we(rd_we),.waddr(rd_wa),.wdata(power_wdata),.raddr(rd_read_address),.rdata(rd_read_data));
 radar_dsp_bram #(.WIDTH(64),.DEPTH(8192),.ADDR_WIDTH(13)) u_enhanced_bram(
  .clk(clk),.we(enhanced_we),.waddr(enhanced_wa),.wdata(enhanced_wdata),.raddr(cfar_address),.rdata(enhanced_read_data));
 radar_dsp_bram #(.WIDTH(64),.DEPTH(256),.ADDR_WIDTH(8)) u_spectrum_bram(
  .clk(clk),.we(spectrum_we),.waddr(spectrum_wa),.wdata(power_wdata),.raddr(spectrum_read_address),.rdata(spectrum_read_data));
 radar_dsp_bram #(.WIDTH(48),.DEPTH(16384),.ADDR_WIDTH(14)) u_tile_bram(
  .clk(clk),.we(tile_we),.waddr(tile_wa),.wdata({read_data[55:32],read_data[23:0]}),.raddr(tile_ra),.rdata(tile_rdata));
 always @(posedge clk) begin
  if(!rst_n)power_pending<=0;
  else begin
   power_pending<=fft_consumed;
   if(state==CLEAR)begin
    local_mask[clear_index[12:0]]<=0;fresh_mask[clear_index[12:0]]<=0;
   end else begin
    if(local_valid&&!cfar_enhanced)local_mask[local_address]<=local_peak;
    if(temporal_result_valid&&temporal_result_ready)fresh_mask[temporal_result_index]<=temporal_fresh;
   end
   if(fft_consumed)begin
    power_is_doppler<=doppler_pass;power_value<=magnitude(fft_output);
    power_index<=doppler_pass?doppler_power_address:{5'd0,out_index[7:0]};
   end
  end
  cfar_gate<=fresh_mask[cfar_address];
 end

 reg [511:0] temporal_header,temporal_cell,temporal_write_state;
 reg temporal_start,temporal_cell_valid;wire temporal_ready,temporal_header_valid,temporal_cell_ready;
 wire [511:0] temporal_header_out,temporal_state_out;wire [31:0] temporal_flags,temporal_cell_flags;
 wire temporal_result_valid,temporal_fresh;wire [12:0] temporal_result_index;wire [63:0] temporal_power;
 wire temporal_result_ready=state==T_CELL_RESULT;
 reg [63:0] temporal_input_power;reg temporal_local;reg temporal_finished,temporal_committed;
 radar_temporal_detect u_temporal(.clk(clk),.rst_n(rst_n),.cancel(state==FINISH&&(cancel_pending||error!=0)),
  .start_frame(temporal_start),.frame_ready(temporal_ready),.frame_header_valid(temporal_header_valid),
  .frame_header_in(temporal_header),.timestamp_us({job[17*32+:32],job[16*32+:32]}),
  .config_epoch(job[7*32+:32]),.acquisition_id(job[19*32+:32]),.frame_index(job[18*32+:32]),
  .validity_flags(job[20*32+:32]),.alpha_q31(job[21*32+:32]),.temporal_control(job[22*32+:32]),
  .hold_threshold_q16(job[25*32+:32]),.frame_header_out(temporal_header_out),.frame_flags(temporal_flags),
  .cell_valid(temporal_cell_valid),.cell_index(cell_index),.power(temporal_input_power),.state_in(temporal_cell),
  .cell_local_max(temporal_local),.cell_ready(temporal_cell_ready),.result_valid(temporal_result_valid),
  .result_index(temporal_result_index),.state_out(temporal_state_out),.enhanced_power(temporal_power),
  .fresh_evidence(temporal_fresh),.result_flags(temporal_cell_flags),.result_ready(temporal_result_ready));

 reg [1:0] output_kind;reg [13:0] output_index,output_words;
 always @* begin
  read_ready=(state==R_COLLECT||state==TILE_COLLECT||state==T_HEADER_GET||state==T_CELL_GET);
  write_data=write_latch;write_valid=state==OUTPUT_PUSH||state==T_ZERO_WAIT||state==T_CELL_WAIT||state==T_COMMIT_WAIT;
  if(feeding)begin write_data=ddr_word(fft_output);write_valid=fft_output_valid;end
  else if(state==T_ZERO_WAIT)begin write_data=0;write_valid=cell_word<8;end
  else if(state==T_CELL_WAIT)begin write_data=temporal_write_state[cell_word*64+:64];write_valid=cell_word<8;end
  else if(state==T_COMMIT_WAIT)begin write_data=temporal_header_out[cell_word*64+:64];write_valid=cell_word<8;end
  point_index=(output_index>=8)?((output_index-8)>>2):0;
 end

 always @(posedge clk) begin
  if(!rst_n) begin
   state<=IDLE;busy<=0;done<=0;error<=0;cycles<=0;candidate_count<=0;overflow_count<=0;vector_count<=0;result_flags<=0;
   job<=0;cancel_pending<=0;clear_index<=0;vector_index<=0;sample_index<=0;prep_index<=0;send_index<=0;out_index<=0;
   tile_base<=0;tile_bank<=0;range_in_tile<=0;channel_index<=0;cell_index<=0;cell_word<=0;
   input_base<=0;range_base<=0;cube_base<=0;rd_base<=0;spectrum_base<=0;candidates_base<=0;temporal_base<=0;
   sum_re<=0;sum_im<=0;mean_re<=0;mean_im<=0;pre_re<=0;pre_im<=0;coefficient<=0;product_re<=0;product_im<=0;
   doppler_pass<=0;fft_input<=0;fft_input_valid<=0;range_configured<=0;doppler_configured<=0;fft_status_seen<=0;
   rd_start<=0;wr_start<=0;rd_address<=0;wr_address<=0;rd_bytes<=0;wr_bytes<=0;write_latch<=0;
   cfar_start<=0;cfar_enhanced<=0;temporal_header<=0;temporal_cell<=0;temporal_write_state<=0;
   temporal_start<=0;temporal_cell_valid<=0;temporal_input_power<=0;temporal_local<=0;temporal_finished<=0;temporal_committed<=0;
   output_kind<=0;output_index<=0;output_words<=0;
  end else begin
   done<=0;rd_start<=0;wr_start<=0;cfar_start<=0;temporal_start<=0;
   if(busy)cycles<=cycles+1;
   if(cancel&&busy)cancel_pending<=1;
   if(rd_done&&rd_error!=0)error<=rd_error;
   if(wr_done&&wr_error!=0)error<=wr_error;
   if((rovf||dovf)&&busy)begin overflow_count<=overflow_count+1;error<=32'h201;end
   if((rtunexpected||rtmissing||dtunexpected||dtmissing)&&busy)error<=32'h202;
   if((rstatus_valid&&!doppler_pass)||(dstatus_valid&&doppler_pass))fft_status_seen<=1;
   if(fft_consumed)begin
    out_index<=out_index+1;
    if(fft_output_last!=(out_index==(doppler_pass?31:255)))error<=32'h203;
   end
   case(state)
    IDLE:if(start&&memory_locked)begin error<=memory_fault;done<=1;end
     else if(start)begin job<=cfg;busy<=1;error<=0;cycles<=0;candidate_count<=0;
     overflow_count<=0;vector_count<=0;result_flags<=0;cancel_pending<=0;state<=CHECK;
     range_configured<=0;doppler_configured<=0;temporal_finished<=0;temporal_committed<=0;cfar_enhanced<=0;end
    CHECK:begin
     if(!address_ok||(job[20*32+:32]&3)!=3)begin error<=32'h001;state<=FINISH;end
     else begin input_base<=bases[0];range_base<=bases[1];cube_base<=bases[2];rd_base<=bases[3];
      spectrum_base<=bases[4];candidates_base<=bases[5];temporal_base<=bases[6];clear_index<=0;state<=CLEAR;end
    end
    CLEAR:if(clear_index==8191)state<=CONFIG;else clear_index<=clear_index+1;
    CONFIG:begin
     if(rcfg_ready)range_configured<=1;if(dcfg_ready)doppler_configured<=1;
     if(range_configured&&doppler_configured)begin vector_index<=0;doppler_pass<=0;state<=R_READ;end
    end
    R_READ:if(cancel_pending||error!=0)state<=FINISH;else begin
     rd_address<=input_base+vector_index*2048;rd_bytes<=2048;rd_start<=1;
     sample_index<=0;sum_re<=0;sum_im<=0;state<=R_COLLECT;end
    R_COLLECT:begin
     if(read_valid)begin
      vector_re[sample_index]<=$signed(read_data[15:0])<<<7;vector_im[sample_index]<=$signed(read_data[47:32])<<<7;
      sum_re<=sum_re+($signed({{24{read_data[15]}},read_data[15:0]})<<<7);
      sum_im<=sum_im+($signed({{24{read_data[47]}},read_data[47:32]})<<<7);
      if(read_data[31:16]!={16{read_data[15]}}||read_data[63:48]!={16{read_data[47]}})error<=32'h002;
      sample_index<=sample_index+1;
     end
     if(rd_done)begin if(cancel_pending||error!=0||rd_error!=0)state<=FINISH;else state<=R_MEAN;end
    end
    R_MEAN:begin mean_re<=rne(sum_re,8);mean_im<=rne(sum_im,8);prep_index<=0;state<=PRE_LOAD;end
    PRE_FETCH:state<=PRE_LOAD;
    PRE_LOAD:begin
     if(doppler_pass)begin
      pre_re<=$signed(tile_rdata[23:0]);pre_im<=$signed(tile_rdata[47:24]);
      coefficient<=window_doppler;
     end else begin
      pre_re<=$signed(vector_re[prep_index])-$signed(mean_re);pre_im<=$signed(vector_im[prep_index])-$signed(mean_im);
      coefficient<=window_range;
     end
     state<=PRE_MUL;
    end
    PRE_MUL:begin product_re<=pre_re*$signed({1'b0,coefficient});product_im<=pre_im*$signed({1'b0,coefficient});state<=PRE_STORE;end
    PRE_STORE:begin
     vector_re[prep_index]<=saturate24(rne(product_re,24));vector_im[prep_index]<=saturate24(rne(product_im,24));
     if(rne(product_re,24)>8388607||rne(product_re,24)< -8388608||rne(product_im,24)>8388607||rne(product_im,24)< -8388608)
      begin overflow_count<=overflow_count+1;error<=32'h204;end
     if(prep_index==(doppler_pass?31:255))state<=doppler_pass?D_WRITE:R_WRITE;
     else begin prep_index<=prep_index+1;state<=doppler_pass?PRE_FETCH:PRE_LOAD;end
    end
    R_WRITE:begin wr_address<=range_base+vector_index*2048;wr_bytes<=2048;wr_start<=1;
     send_index<=0;out_index<=0;fft_status_seen<=0;state<=F_LOAD;end
    D_WRITE:begin wr_address<=cube_base+((tile_base+range_in_tile)*12+channel_index)*256;wr_bytes<=256;wr_start<=1;
     send_index<=0;out_index<=0;fft_status_seen<=0;state<=F_LOAD;end
    F_LOAD:begin fft_input<={vector_im[send_index],vector_re[send_index]};fft_input_valid<=1;state<=F_PUSH;end
    F_PUSH:if(fft_input_ready)begin
     fft_input_valid<=0;
     if(send_index==(doppler_pass?31:255))state<=F_WAIT;
     else begin send_index<=send_index+1;state<=F_LOAD;end
    end
    F_WAIT:if(wr_done)begin
     vector_count<=vector_count+1;
     if(!fft_status_seen)error<=32'h205;
     if(cancel_pending||error!=0||wr_error!=0)state<=FINISH;
     else if(!doppler_pass)begin
      if(vector_index==383)begin tile_base<=0;tile_bank<=0;state<=TILE_START;end
      else begin vector_index<=vector_index+1;state<=R_READ;end
     end else if(channel_index<11)begin channel_index<=channel_index+1;prep_index<=0;state<=PRE_FETCH;end
     else if(range_in_tile<15)begin channel_index<=0;range_in_tile<=range_in_tile+1;prep_index<=0;state<=PRE_FETCH;end
     else if(tile_base<240)begin tile_base<=tile_base+16;tile_bank<=!tile_bank;state<=TILE_START;end
     else begin output_kind<=0;state<=OUTPUT_START;end
    end
    TILE_START:begin vector_index<=0;state<=TILE_READ;end
    TILE_READ:if(cancel_pending||error!=0)state<=FINISH;else begin
     rd_address<=range_base+vector_index*2048+tile_base*8;rd_bytes<=128;rd_start<=1;sample_index<=0;state<=TILE_COLLECT;end
    TILE_COLLECT:begin
     if(read_valid)begin
      sample_index<=sample_index+1;
     end
     if(rd_done)begin if(vector_index==383)state<=D_PREP;else begin vector_index<=vector_index+1;state<=TILE_READ;end end
    end
    D_PREP:begin doppler_pass<=1;range_in_tile<=0;channel_index<=0;prep_index<=0;state<=PRE_FETCH;end
    OUTPUT_START:begin
     wr_address<=output_kind==0?spectrum_base:(output_kind==1?rd_base:candidates_base);
     wr_bytes<=output_kind==0?2048:(output_kind==1?65536:1088);
     output_words<=output_kind==0?256:(output_kind==1?8192:136);output_index<=0;wr_start<=1;state<=OUTPUT_FETCH;
    end
    OUTPUT_FETCH:state<=OUTPUT_LOAD;
    OUTPUT_LOAD:begin
     if(output_kind==0)write_latch<=spectrum_read_data;
     else if(output_kind==1)write_latch<=rd_read_data;
     else if(output_index<8)case(output_index)
      0:write_latch<={cfar_total,cfar_count};1:write_latch<={error,cfar_total-cfar_count};
      2:write_latch<={vector_count,overflow_count};3:write_latch<=cycles;
      4:write_latch<={job[7*32+:32],job[6*32+:32]};5:write_latch<=result_flags;
      default:write_latch<=0;endcase
     else if(point_index>=cfar_count)write_latch<=0;
     else case(output_index[1:0])
      0:write_latch<={point_doppler,point_range};1:write_latch<=point_power;
      2:write_latch<=point_noise;3:write_latch<={32'd0,point_noise_count};endcase
     state<=OUTPUT_PUSH;
    end
    OUTPUT_PUSH:if(wr_ready)begin
     if(output_index+1==output_words)state<=OUTPUT_WAIT;else begin output_index<=output_index+1;state<=OUTPUT_FETCH;end end
    OUTPUT_WAIT:if(wr_done)begin
     if(cancel_pending||error!=0||wr_error!=0)state<=FINISH;
     else if(output_kind==0)begin output_kind<=1;state<=OUTPUT_START;end
     else if(output_kind==1)state<=CFAR_START;
     else state<=FINISH;
    end
    CFAR_START:begin cfar_start<=1;state<=CFAR_WAIT;end
    CFAR_WAIT:if(cfar_done)begin
     candidate_count<=cfar_count;
     if(cancel_pending||error!=0)state<=FINISH;
     else if(job[5*32+:32]==2&&!temporal_finished)state<=T_HEADER_READ;
     else begin output_kind<=2;state<=OUTPUT_START;end
    end
    T_HEADER_READ:begin rd_address<=temporal_base;rd_bytes<=64;rd_start<=1;cell_word<=0;state<=T_HEADER_GET;end
    T_HEADER_GET:begin
     if(read_valid)begin temporal_header[cell_word*64+:64]<=read_data;cell_word<=cell_word+1;end
     if(rd_done)state<=T_BEGIN;
    end
    T_BEGIN:if(temporal_ready)begin temporal_start<=1;state<=T_BEGIN_WAIT;end
    T_BEGIN_WAIT:if(temporal_header_valid)begin
     result_flags<=temporal_flags;
     if((temporal_flags&32'hc)!=0)begin error<=32'h301;candidate_count<=0;end
     state<=T_ZERO_START;
    end
    T_ZERO_START:begin wr_address<=temporal_base;wr_bytes<=64;wr_start<=1;cell_word<=0;state<=T_ZERO_WAIT;end
    T_ZERO_WAIT:begin
     if(wr_ready&&cell_word<8)cell_word<=cell_word+1;
     if(wr_done)begin
      temporal_committed<=0;cell_index<=0;
      if(cancel_pending||error!=0||wr_error!=0)state<=FINISH;else state<=T_CELL_READ;
     end
    end
    T_CELL_READ:if(cancel_pending||error!=0)state<=FINISH;else begin
     rd_address<=temporal_base+64+cell_index*64;rd_bytes<=64;rd_start<=1;cell_word<=0;
     temporal_local<=local_mask[cell_index];state<=T_CELL_GET;end
    T_CELL_GET:begin
     temporal_input_power<=rd_read_data;
     if(read_valid)begin temporal_cell[cell_word*64+:64]<=read_data;cell_word<=cell_word+1;end
     if(rd_done)begin temporal_cell_valid<=1;state<=T_CELL_SEND;end
    end
    T_CELL_SEND:if(temporal_cell_ready)begin temporal_cell_valid<=0;state<=T_CELL_RESULT;end
    T_CELL_RESULT:if(temporal_result_valid)begin
     temporal_write_state<=temporal_state_out;result_flags<=result_flags|temporal_cell_flags;
     if(temporal_result_index!=cell_index||(temporal_cell_flags&32'h200)!=0)error<=32'h302;
     if((temporal_cell_flags&32'h100)!=0)begin
      error<=32'h303;overflow_count<=overflow_count+1;candidate_count<=0;
     end
     state<=T_CELL_WRITE;
    end
    T_CELL_WRITE:begin wr_address<=temporal_base+64+cell_index*64;wr_bytes<=64;wr_start<=1;cell_word<=0;state<=T_CELL_WAIT;end
    T_CELL_WAIT:begin
     if(wr_ready&&cell_word<8)cell_word<=cell_word+1;
     if(wr_done)begin
      if(cell_index==8191)state<=T_COMMIT;
      else begin cell_index<=cell_index+1;state<=T_CELL_READ;end
     end
    end
    T_COMMIT:if(cancel_pending||error!=0)state<=FINISH;else begin
     wr_address<=temporal_base;wr_bytes<=64;wr_start<=1;cell_word<=0;state<=T_COMMIT_WAIT;end
    T_COMMIT_WAIT:begin
     if(wr_ready&&cell_word<8)cell_word<=cell_word+1;
     if(wr_done)begin
      temporal_finished<=1;temporal_committed<=1;
      if(cancel_pending||error!=0||wr_error!=0)state<=FINISH;
      else if((result_flags&32'h40)!=0)begin output_kind<=2;state<=OUTPUT_START;end
      else begin cfar_enhanced<=1;state<=CFAR_START;end
     end
    end
    FINISH:if(!rd_busy&&!wr_busy&&!rd_start&&!wr_start)begin
     if((cancel_pending||error!=0)&&temporal_committed)state<=T_ZERO_START;
     else begin
      if(cancel_pending)begin error<=32'h004;candidate_count<=0;end
      busy<=0;done<=1;state<=IDLE;
     end
    end
    FAULT_DRAIN:if(memory_quiescent)begin
     error<=memory_fault;candidate_count<=0;result_flags<=result_flags|32'h80000000;
     busy<=0;done<=1;state<=IDLE;
    end
    default:begin error<=32'hfff;state<=FINISH;end
   endcase
   if(memory_locked&&busy&&state!=FAULT_DRAIN)begin
    state<=FAULT_DRAIN;error<=memory_fault;rd_start<=0;wr_start<=0;fft_input_valid<=0;temporal_cell_valid<=0;
   end
  end
 end
endmodule
