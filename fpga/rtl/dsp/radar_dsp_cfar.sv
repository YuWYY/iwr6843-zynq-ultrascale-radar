`timescale 1ns/1ps
// Exact original window/boundary/tie rules. Detector and physical raw power
// are separate so an enhanced dimensionless map cannot become ADC dBFS.
module radar_dsp_cfar(
 input wire clk,rst_n,start,input wire [31:0] threshold_q16,input wire enhanced,
 output reg busy,done,output reg [12:0] address,
 input wire [63:0] detector_data,raw_data,input wire fresh_gate,
 output reg local_valid,output reg [12:0] local_address,output reg local_peak,
 output reg [31:0] count,total,
 input wire [4:0] result_index,
 output wire [31:0] result_range,result_doppler,
 output wire [63:0] result_power,result_noise,
 output wire [31:0] result_noise_count
);
 localparam IDLE=0,CUT_REQ=1,CUT_WAIT=2,CUT_GET=3,NEXT_N=4,N_WAIT=5,N_GET=6,
            NEXT_T=7,T_WAIT=8,T_GET=9,DECIDE=10,INSERT=11,NEXT_CUT=12;
 reg [3:0] state; reg [7:0] r;reg [4:0] k;
 reg signed [5:0] dr,dk;integer rr,kk;reg is_peak;
 reg [63:0] cut,raw_cut,raw_sum;reg [71:0] sum;reg [8:0] ntrain;
 reg [31:0] r_store[0:31],k_store[0:31],n_store[0:31];
 reg [63:0] rank_store[0:31],p_store[0:31],noise_store[0:31];
 reg [5:0] insert_at; integer i;
 // UQ32 enhanced cells may saturate near2^64. Up to216 training cells
 // need72bits; multiplying by a32bit Q16 threshold needs104bits.
 wire [111:0] lhs=({48'd0,cut}*ntrain)<<16;
 wire [111:0] rhs={40'd0,sum}*threshold_q16;
 assign result_range=r_store[result_index];assign result_doppler=k_store[result_index];
 assign result_power=p_store[result_index];assign result_noise=noise_store[result_index];
 assign result_noise_count=n_store[result_index];
 always @* begin rr=$signed({1'b0,r})+dr;kk=($signed({1'b0,k})+dk+32)&31;end
 always @(posedge clk) begin
  if(!rst_n) begin state<=IDLE;busy<=0;done<=0;address<=0;local_valid<=0;
   local_address<=0;local_peak<=0;count<=0;total<=0;r<=6;k<=0;dr<=0;dk<=0;
   cut<=0;raw_cut<=0;sum<=0;raw_sum<=0;ntrain<=0;is_peak<=0;insert_at<=0;
   for(i=0;i<32;i=i+1) begin r_store[i]<=0;k_store[i]<=0;n_store[i]<=0;
     rank_store[i]<=0;p_store[i]<=0;noise_store[i]<=0;end
  end else begin
   done<=0;local_valid<=0;
   case(state)
    IDLE: if(start) begin busy<=1;count<=0;total<=0;r<=6;k<=0;state<=CUT_REQ;
      for(i=0;i<32;i=i+1) begin r_store[i]<=0;k_store[i]<=0;n_store[i]<=0;
       rank_store[i]<=0;p_store[i]<=0;noise_store[i]<=0;end end
    CUT_REQ: begin address<={r,k};state<=CUT_WAIT;end
    CUT_WAIT: state<=CUT_GET;
    CUT_GET: begin cut<=detector_data;raw_cut<=raw_data;is_peak<=detector_data!=0;
      dr<=-1;dk<=-1;state<=NEXT_N;end
    NEXT_N: begin
      if(dr==2) begin
       local_valid<=1;local_address<={r,k};local_peak<=is_peak;
       // fresh_gate corresponds to the CUT, obtained again below.
       if(is_peak) begin address<={r,k};state<=NEXT_T;dr<=-10;dk<=-5;
         sum<=0;raw_sum<=0;ntrain<=0;end else state<=NEXT_CUT;
      end else if(rr<6||rr>204||(dr==0&&dk==0)) begin
       if(dk==1) begin dk<=-1;dr<=dr+1;end else dk<=dk+1;
      end else begin address<=rr*32+kk;state<=N_WAIT;end
    end
    N_WAIT: state<=N_GET;
    N_GET: begin
      if(detector_data>cut || (detector_data==cut && ((rr<r)||(rr==r&&kk<k)))) is_peak<=0;
      if(dk==1) begin dk<=-1;dr<=dr+1;end else dk<=dk+1;
      state<=NEXT_N;
    end
    NEXT_T: begin
      if(dr==11) begin address<={r,k};state<=DECIDE;end
      else if(rr<6||rr>204||(dr>=-2&&dr<=2&&dk>=-1&&dk<=1)) begin
       if(dk==5) begin dk<=-5;dr<=dr+1;end else dk<=dk+1;
      end else begin address<=rr*32+kk;state<=T_WAIT;end
    end
    T_WAIT:state<=T_GET;
    T_GET:begin sum<=sum+detector_data;raw_sum<=raw_sum+raw_data;ntrain<=ntrain+1;
      if(dk==5) begin dk<=-5;dr<=dr+1;end else dk<=dk+1;
      state<=NEXT_T;end
    DECIDE:begin // one extra cycle also returns the CUT's fresh gate
      state<=INSERT;insert_at<=0;
    end
    INSERT:begin
      if(ntrain!=0 && lhs>rhs && (!enhanced||fresh_gate)) begin
       if(insert_at<count && insert_at<32 &&
          (rank_store[insert_at]>cut || (rank_store[insert_at]==cut &&
           (r_store[insert_at]<r||(r_store[insert_at]==r&&k_store[insert_at]<k)))))
          insert_at<=insert_at+1;
       else begin
        total<=total+1;
        if(insert_at<32) begin
         for(i=31;i>0;i=i-1) if(i>insert_at) begin
          r_store[i]<=r_store[i-1];k_store[i]<=k_store[i-1];n_store[i]<=n_store[i-1];
          rank_store[i]<=rank_store[i-1];p_store[i]<=p_store[i-1];noise_store[i]<=noise_store[i-1];end
         r_store[insert_at]<=r;k_store[insert_at]<=k;n_store[insert_at]<=ntrain;
         rank_store[insert_at]<=cut;p_store[insert_at]<=raw_cut;noise_store[insert_at]<=raw_sum;
         if(count<32)count<=count+1;
        end
        state<=NEXT_CUT;
       end
      end else state<=NEXT_CUT;
    end
    NEXT_CUT:if(k==31) begin k<=0;if(r==204)begin busy<=0;done<=1;state<=IDLE;end
      else begin r<=r+1;state<=CUT_REQ;end end else begin k<=k+1;state<=CUT_REQ;end
    default:state<=IDLE;
   endcase
  end
 end
endmodule
