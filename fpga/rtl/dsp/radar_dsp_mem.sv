`timescale 1ns/1ps
// One outstanding transfer per direction. Completion includes BOTH payload
// and DataMover status; addresses/counts are supplied only by the job engine.
module radar_dsp_mem(
 input wire clk,rst_n,
 output reg fault_locked,output wire fault_quiescent,output reg [31:0] fault_code,
 output wire mm2s_halt,s2mm_halt,input wire mm2s_halt_cmplt,s2mm_halt_cmplt,
 input wire rd_start,input wire [31:0] rd_addr,input wire [22:0] rd_bytes,
 output reg rd_busy,rd_done,output reg [31:0] rd_error,
 output wire [63:0] rd_data,output wire rd_valid,input wire rd_ready,
 input wire wr_start,input wire [31:0] wr_addr,input wire [22:0] wr_bytes,
 output reg wr_busy,wr_done,output reg [31:0] wr_error,
 input wire [63:0] wr_data,input wire wr_valid,output wire wr_ready,
 output wire [71:0] mm2s_cmd_tdata,output reg mm2s_cmd_tvalid,input wire mm2s_cmd_tready,
 input wire [7:0] mm2s_sts_tdata,input wire mm2s_sts_tvalid,output wire mm2s_sts_tready,
 input wire [63:0] mm2s_tdata,input wire mm2s_tvalid,output wire mm2s_tready,input wire mm2s_tlast,
 output wire [71:0] s2mm_cmd_tdata,output reg s2mm_cmd_tvalid,input wire s2mm_cmd_tready,
 input wire [7:0] s2mm_sts_tdata,input wire s2mm_sts_tvalid,output wire s2mm_sts_tready,
 output wire [63:0] s2mm_tdata,output wire s2mm_tvalid,input wire s2mm_tready,output wire s2mm_tlast
);
 reg [31:0] ra,wa; reg [22:0] rb,wb,rc,wc;
 reg rissued,wissued,rstatus,wstatus,rpayload,wpayload;
 reg held_write_valid;reg [63:0] held_write_data;reg held_write_last;
 assign mm2s_halt=fault_locked;assign s2mm_halt=fault_locked;
 assign fault_quiescent=fault_locked&&mm2s_halt_cmplt&&s2mm_halt_cmplt;
 assign mm2s_cmd_tdata={4'd0,4'd0,ra,1'b0,1'b1,6'd0,1'b1,rb};
 assign s2mm_cmd_tdata={4'd0,4'd0,wa,1'b0,1'b1,6'd0,1'b1,wb};
 assign rd_data=mm2s_tdata;
 assign rd_valid=!fault_locked&&rd_busy&&rissued&&!rpayload&&mm2s_tvalid;
 assign mm2s_tready=fault_locked?!mm2s_halt_cmplt:(rd_busy&&rissued&&!rpayload&&rd_ready);
 assign mm2s_sts_tready=rd_busy&&rissued&&!rstatus;
 assign s2mm_tdata=fault_locked?held_write_data:wr_data;
 assign s2mm_tvalid=fault_locked?(held_write_valid&&!s2mm_halt_cmplt):(wr_busy&&wissued&&!wpayload&&wr_valid);
 assign wr_ready=!fault_locked&&wr_busy&&wissued&&!wpayload&&s2mm_tready;
 assign s2mm_tlast=fault_locked?held_write_last:(wc+23'd8==wb);
 assign s2mm_sts_tready=wr_busy&&wissued&&!wstatus;
 always @(posedge clk) begin
  if(!rst_n) begin
   rd_busy<=0;rd_done<=0;rd_error<=0;wr_busy<=0;wr_done<=0;wr_error<=0;
   ra<=0;wa<=0;rb<=0;wb<=0;rc<=0;wc<=0; rissued<=0;wissued<=0;
   rstatus<=0;wstatus<=0;rpayload<=0;wpayload<=0;
   mm2s_cmd_tvalid<=0;s2mm_cmd_tvalid<=0;
   fault_locked<=0;fault_code<=0;held_write_valid<=0;held_write_data<=0;held_write_last<=0;
  end else begin
   rd_done<=0;wr_done<=0;
   if(rd_start&&!rd_busy&&!fault_locked) begin
    ra<=rd_addr;rb<=rd_bytes;rc<=0;rd_busy<=1;rd_error<=0;
    rissued<=0;rstatus<=0;rpayload<=0;mm2s_cmd_tvalid<=1;
   end
   if(wr_start&&!wr_busy&&!fault_locked) begin
    wa<=wr_addr;wb<=wr_bytes;wc<=0;wr_busy<=1;wr_error<=0;
    wissued<=0;wstatus<=0;wpayload<=0;s2mm_cmd_tvalid<=1;
   end
   if(mm2s_cmd_tvalid&&mm2s_cmd_tready) begin mm2s_cmd_tvalid<=0;rissued<=1;end
   if(s2mm_cmd_tvalid&&s2mm_cmd_tready) begin s2mm_cmd_tvalid<=0;wissued<=1;end
   if(rd_valid&&rd_ready) begin
    rc<=rc+8;
    if(mm2s_tlast!=(rc+23'd8==rb))begin
     rd_error<=32'h80000101;fault_locked<=1;fault_code<=32'h80000101;
     held_write_valid<=s2mm_tvalid&&!s2mm_tready;held_write_data<=s2mm_tdata;held_write_last<=s2mm_tlast;
    end
    if(rc+23'd8==rb) rpayload<=1;
   end
   if(s2mm_tvalid&&s2mm_tready) begin
    if(fault_locked)held_write_valid<=0;
    else begin wc<=wc+8;if(wc+23'd8==wb) wpayload<=1;end
   end
   if(mm2s_sts_tvalid&&mm2s_sts_tready) begin
    rstatus<=1;if(mm2s_sts_tdata!=8'h80&&!fault_locked)begin
     rd_error<={24'h800102,mm2s_sts_tdata};fault_code<={24'h800102,mm2s_sts_tdata};fault_locked<=1;
     held_write_valid<=s2mm_tvalid&&!s2mm_tready;held_write_data<=s2mm_tdata;held_write_last<=s2mm_tlast;
    end
   end
   if(s2mm_sts_tvalid&&s2mm_sts_tready) begin
    wstatus<=1;if(s2mm_sts_tdata!=8'h80&&!fault_locked)begin
     wr_error<={24'h800103,s2mm_sts_tdata};fault_code<={24'h800103,s2mm_sts_tdata};fault_locked<=1;
     held_write_valid<=s2mm_tvalid&&!s2mm_tready;held_write_data<=s2mm_tdata;held_write_last<=s2mm_tlast;
    end
   end
   if(!fault_locked&&rd_busy&&rpayload&&rstatus&&rd_error==0) begin rd_busy<=0;rd_done<=1;end
   if(!fault_locked&&wr_busy&&wpayload&&wstatus&&wr_error==0) begin wr_busy<=0;wr_done<=1;end
   if(fault_quiescent)begin rd_busy<=0;wr_busy<=0;mm2s_cmd_tvalid<=0;s2mm_cmd_tvalid<=0;held_write_valid<=0;end
  end
 end
endmodule
