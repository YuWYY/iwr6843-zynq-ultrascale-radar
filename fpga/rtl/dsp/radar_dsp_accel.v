// SPDX-License-Identifier: MIT
// AXI-Lite control and DataMover stream boundary for the offline-validated DSP.
// Address/format ABI is documented in verification/20261101_pl_dsp_01/CONTRACT.md.
`timescale 1ns/1ps
module radar_dsp_accel (
    (* X_INTERFACE_INFO = "xilinx.com:signal:clock:1.0 aclk CLK" *)
    (* X_INTERFACE_PARAMETER = "XIL_INTERFACENAME aclk, ASSOCIATED_BUSIF S_AXI:M_AXIS_MM2S_CMD:S_AXIS_MM2S_STS:S_AXIS_MM2S:M_AXIS_S2MM_CMD:S_AXIS_S2MM_STS:M_AXIS_S2MM, ASSOCIATED_RESET aresetn:dsp_datamover_aresetn, FREQ_HZ 99999001" *)
    input wire aclk,
    (* X_INTERFACE_INFO = "xilinx.com:signal:reset:1.0 aresetn RST" *)
    (* X_INTERFACE_PARAMETER = "XIL_INTERFACENAME aresetn, POLARITY ACTIVE_LOW" *)
    input wire aresetn,
    (* X_INTERFACE_INFO = "xilinx.com:signal:reset:1.0 dsp_datamover_aresetn RST" *)
    (* X_INTERFACE_PARAMETER = "XIL_INTERFACENAME dsp_datamover_aresetn, POLARITY ACTIVE_LOW" *)
    output wire dsp_datamover_aresetn,
    output wire mm2s_halt,
    output wire s2mm_halt,
    input wire mm2s_halt_cmplt,
    input wire s2mm_halt_cmplt,
    // Passive observation at FIFO->DMA. Never drives capture backpressure.
    input wire capture_valid_observe,
    input wire capture_ready_observe,
    input wire capture_last_observe,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI AWADDR" *)
    input wire [15:0] s_axi_awaddr,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI AWPROT" *)
    input wire [2:0] s_axi_awprot,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI AWVALID" *)
    input wire s_axi_awvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI AWREADY" *)
    output wire s_axi_awready,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI WDATA" *)
    input wire [31:0] s_axi_wdata,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI WSTRB" *)
    input wire [3:0] s_axi_wstrb,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI WVALID" *)
    input wire s_axi_wvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI WREADY" *)
    output wire s_axi_wready,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI BRESP" *)
    output reg [1:0] s_axi_bresp,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI BVALID" *)
    output reg s_axi_bvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI BREADY" *)
    input wire s_axi_bready,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI ARADDR" *)
    input wire [15:0] s_axi_araddr,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI ARPROT" *)
    input wire [2:0] s_axi_arprot,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI ARVALID" *)
    input wire s_axi_arvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI ARREADY" *)
    output wire s_axi_arready,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI RDATA" *)
    output reg [31:0] s_axi_rdata,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI RRESP" *)
    output reg [1:0] s_axi_rresp,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI RVALID" *)
    output reg s_axi_rvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI RREADY" *)
    input wire s_axi_rready,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS_MM2S_CMD TDATA" *)
    output wire [71:0] m_axis_mm2s_cmd_tdata,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS_MM2S_CMD TVALID" *)
    output wire m_axis_mm2s_cmd_tvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS_MM2S_CMD TREADY" *)
    input wire m_axis_mm2s_cmd_tready,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S_AXIS_MM2S_STS TDATA" *)
    input wire [7:0] s_axis_mm2s_sts_tdata,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S_AXIS_MM2S_STS TVALID" *)
    input wire s_axis_mm2s_sts_tvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S_AXIS_MM2S_STS TREADY" *)
    output wire s_axis_mm2s_sts_tready,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S_AXIS_MM2S TDATA" *)
    input wire [63:0] s_axis_mm2s_tdata,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S_AXIS_MM2S TVALID" *)
    input wire s_axis_mm2s_tvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S_AXIS_MM2S TREADY" *)
    output wire s_axis_mm2s_tready,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S_AXIS_MM2S TLAST" *)
    input wire s_axis_mm2s_tlast,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS_S2MM_CMD TDATA" *)
    output wire [71:0] m_axis_s2mm_cmd_tdata,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS_S2MM_CMD TVALID" *)
    output wire m_axis_s2mm_cmd_tvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS_S2MM_CMD TREADY" *)
    input wire m_axis_s2mm_cmd_tready,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S_AXIS_S2MM_STS TDATA" *)
    input wire [7:0] s_axis_s2mm_sts_tdata,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S_AXIS_S2MM_STS TVALID" *)
    input wire s_axis_s2mm_sts_tvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S_AXIS_S2MM_STS TREADY" *)
    output wire s_axis_s2mm_sts_tready,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS_S2MM TDATA" *)
    output wire [63:0] m_axis_s2mm_tdata,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS_S2MM TKEEP" *)
    output wire [7:0] m_axis_s2mm_tkeep,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS_S2MM TVALID" *)
    output wire m_axis_s2mm_tvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS_S2MM TREADY" *)
    input wire m_axis_s2mm_tready,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS_S2MM TLAST" *)
    output wire m_axis_s2mm_tlast);
    reg [31:0] regs [0:31];
    reg [1023:0] job_cfg;
    reg aw_pending, w_pending;
    reg [15:0] awaddr;
    reg [31:0] wdata;
    reg [3:0] wstrb;
    reg start, cancel, launch_pending, done_latched;
    reg [4:0] recovery_cycles;
    reg recovery_active;
    reg [31:0] error_latched;
    reg meta_pop, meta_clear;
    reg time_live, time_snapshot_valid, time_rejected;
    reg [31:0] time_epoch_token, time_epoch, time_snapshot_epoch, time_generation;
    reg [63:0] time_snapshot_ticks;
    wire [31:0] meta_status, meta_sequence, meta_head_sequence, meta_bytes;
    wire [63:0] meta_first, meta_last, meta_ticks;
    wire capture_accepted = capture_valid_observe && capture_ready_observe;
    wire busy, done;
    wire [31:0] error, candidate_count, overflow_count, vector_count, result_flags;
    wire [63:0] cycles;
    wire occupied = busy | launch_pending | recovery_active;
    integer i, b;

    assign s_axi_awready = !aw_pending && !s_axi_bvalid;
    assign s_axi_wready = !w_pending && !s_axi_bvalid;
    assign s_axi_arready = !s_axi_rvalid;
    assign m_axis_s2mm_tkeep = 8'hff;
    // Only an idle, explicitly requested recovery may reset this AXI master.
    // The engine leaves BUSY only after all transactions have completed/halted.
    assign dsp_datamover_aresetn = aresetn && !recovery_active;

    function [31:0] read_word;
        input [15:0] addr;
        begin
            case(addr)
                16'h0000: read_word=32'h52445031;
                16'h0004: read_word=32'h00010001;
                16'h0008: read_word=0;
                16'h000c: read_word={29'b0,(error_latched!=0),done_latched,occupied};
                16'h0010: read_word=error_latched;
                16'h0068: read_word=cycles[31:0];
                16'h006c: read_word=cycles[63:32];
                16'h0070: read_word=candidate_count;
                16'h0074: read_word=overflow_count;
                16'h0078: read_word=vector_count;
                16'h007c: read_word=result_flags;
                16'h0100: read_word=meta_status;
                16'h0104: read_word=meta_sequence;
                16'h0108: read_word=meta_head_sequence;
                16'h010c: read_word=meta_first[31:0];
                16'h0110: read_word=meta_first[63:32];
                16'h0114: read_word=meta_last[31:0];
                16'h0118: read_word=meta_last[63:32];
                16'h011c: read_word=0;
                16'h0120: read_word=meta_bytes;
                16'h0124: read_word=32'd99999001;
                16'h0128: read_word=0;
                16'h012c: read_word=time_generation;
                16'h0130: read_word=time_snapshot_ticks[31:0];
                16'h0134: read_word=time_snapshot_ticks[63:32];
                16'h0138: read_word=time_snapshot_epoch;
                16'h013c: read_word={27'b0,meta_status[29],meta_status[30],time_rejected,time_snapshot_valid,time_live};
                16'h0140: read_word=time_epoch_token;
                16'h0144: read_word=time_epoch;
                default: read_word=(addr>=16'h0014 && addr<=16'h0064) ? regs[addr[6:2]] : 0;
            endcase
        end
    endfunction

    always @(posedge aclk) begin
        if(!aresetn) begin
            aw_pending<=0; w_pending<=0; awaddr<=0; wdata<=0; wstrb<=0;
            s_axi_bvalid<=0; s_axi_bresp<=0;
            s_axi_rvalid<=0; s_axi_rresp<=0; s_axi_rdata<=0;
            start<=0; cancel<=0; launch_pending<=0;
            recovery_cycles<=0;recovery_active<=0;
            meta_pop<=0;meta_clear<=0;
            time_live<=0;time_snapshot_valid<=0;time_rejected<=0;
            time_epoch_token<=0;time_epoch<=0;time_snapshot_epoch<=0;
            time_generation<=0;time_snapshot_ticks<=0;
            done_latched<=0; error_latched<=0; job_cfg<=0;
            for(i=0;i<32;i=i+1) regs[i]<=0;
            regs[5]<=1;
        end else begin
            start<=0; cancel<=0;
            if(recovery_cycles!=0) recovery_cycles<=recovery_cycles-1'b1;
            if(recovery_cycles==1) recovery_active<=0;
            meta_pop<=0;meta_clear<=0;
            if(busy) launch_pending<=0;
            if(done) begin
                launch_pending<=0; done_latched<=1; error_latched<=error;
            end
            if(s_axi_bvalid && s_axi_bready) s_axi_bvalid<=0;
            if(s_axi_rvalid && s_axi_rready) s_axi_rvalid<=0;
            if(s_axi_awvalid && s_axi_awready) begin awaddr<=s_axi_awaddr;aw_pending<=1;end
            if(s_axi_wvalid && s_axi_wready) begin wdata<=s_axi_wdata;wstrb<=s_axi_wstrb;w_pending<=1;end
            if(aw_pending && w_pending && !s_axi_bvalid) begin
                aw_pending<=0;w_pending<=0;s_axi_bvalid<=1;s_axi_bresp<=0;
                if(awaddr[1:0]!=0) s_axi_bresp<=2'b10;
                else if(awaddr==16'h011c) begin
                    if(wstrb[0]) begin
                        if(wdata==1 && meta_status[6:0]!=0) meta_pop<=1;
                        else if(wdata==2 && !time_live && !capture_accepted) meta_clear<=1;
                        else begin s_axi_bresp<=2'b10;time_rejected<=1;end
                    end
                end else if(awaddr==16'h0140) begin
                    if(wstrb==4'hf && !time_live && !capture_accepted && wdata!=0)
                        time_epoch_token<=wdata;
                    else begin s_axi_bresp<=2'b10;time_rejected<=1;end
                end else if(awaddr==16'h0128) begin
                    if(wstrb!=4'hf) begin s_axi_bresp<=2'b10;time_rejected<=1;end
                    else if(wdata==1) begin
                        // One atomic image of the exact counter used by records.
                        // POP, packet completion and snapshot may share this edge.
                        time_snapshot_ticks<=meta_ticks;time_snapshot_epoch<=time_epoch;
                        time_generation<=time_generation+1'b1;time_snapshot_valid<=1;
                    end else if(wdata==2 && !time_live && !capture_accepted && time_epoch_token!=0) begin
                        // Software invokes this only before ARM, with DMA stopped.
                        meta_clear<=1;time_epoch<=time_epoch_token;time_live<=1;
                        time_snapshot_valid<=0;time_rejected<=0;
                    end else if(wdata==3 && time_live && !capture_accepted) begin
                        // Explicit ownership release after source stop + DMA halt.
                        // Retain even a partial record for abort diagnostics.
                        time_live<=0;
                    end else begin s_axi_bresp<=2'b10;time_rejected<=1;end
                end else if(awaddr==16'h0008) begin
                    if(wstrb[0]) begin
                        // Control commands are exclusive; START freezes all configuration.
                        if(wdata[7:0]==8'h01 && !occupied) begin
                            for(i=0;i<32;i=i+1) job_cfg[i*32+:32]<=regs[i];
                            start<=1;launch_pending<=1;done_latched<=0;error_latched<=0;
                        end else if(wdata[7:0]==8'h02 && occupied) cancel<=1;
                        else if(wdata[7:0]==8'h04 && !occupied) begin done_latched<=0;error_latched<=0;end
                        else if(wdata[7:0]==8'h08 && !occupied) begin
                            recovery_cycles<=16;recovery_active<=1;done_latched<=0;error_latched<=0;
                        end
                        else if(wdata[7:0]!=0) s_axi_bresp<=2'b10;
                    end
                end else if(awaddr>=16'h0014 && awaddr<=16'h0064 && !occupied) begin
                    for(b=0;b<4;b=b+1) if(wstrb[b]) regs[awaddr[6:2]][b*8+:8]<=wdata[b*8+:8];
                end else s_axi_bresp<=2'b10;
            end
            if(s_axi_arvalid && s_axi_arready) begin
                s_axi_rvalid<=1;
                if(s_axi_araddr[1:0]!=0 || (s_axi_araddr>16'h007c && (s_axi_araddr<16'h0100 || s_axi_araddr>16'h0144))) begin
                    s_axi_rdata<=0;s_axi_rresp<=2'b10;
                end else begin s_axi_rdata<=read_word(s_axi_araddr);s_axi_rresp<=0;end
            end
        end
    end

    radar_frame_metadata metadata (
        .clk(aclk),.rst_n(aresetn),.stream_valid(capture_valid_observe),
        .stream_ready(capture_ready_observe),.stream_last(capture_last_observe),
        .pop(meta_pop),.clear(meta_clear),.status(meta_status),
        .next_sequence(meta_sequence),.head_sequence(meta_head_sequence),
        .head_first(meta_first),.head_last(meta_last),.head_bytes(meta_bytes),.current_ticks(meta_ticks)
    );

    radar_dsp_engine engine (
        .clk(aclk),.rst_n(dsp_datamover_aresetn),.start(start),.cancel(cancel),.cfg(job_cfg),
        .mm2s_halt(mm2s_halt),.s2mm_halt(s2mm_halt),
        .mm2s_halt_cmplt(mm2s_halt_cmplt),.s2mm_halt_cmplt(s2mm_halt_cmplt),
        .busy(busy),.done(done),.error(error),.cycles(cycles),
        .candidate_count(candidate_count),.overflow_count(overflow_count),
        .vector_count(vector_count),.result_flags(result_flags),
        .mm2s_cmd_tdata(m_axis_mm2s_cmd_tdata),.mm2s_cmd_tvalid(m_axis_mm2s_cmd_tvalid),
        .mm2s_cmd_tready(m_axis_mm2s_cmd_tready),.mm2s_sts_tdata(s_axis_mm2s_sts_tdata),
        .mm2s_sts_tvalid(s_axis_mm2s_sts_tvalid),.mm2s_sts_tready(s_axis_mm2s_sts_tready),
        .mm2s_tdata(s_axis_mm2s_tdata),.mm2s_tvalid(s_axis_mm2s_tvalid),
        .mm2s_tready(s_axis_mm2s_tready),.mm2s_tlast(s_axis_mm2s_tlast),
        .s2mm_cmd_tdata(m_axis_s2mm_cmd_tdata),.s2mm_cmd_tvalid(m_axis_s2mm_cmd_tvalid),
        .s2mm_cmd_tready(m_axis_s2mm_cmd_tready),.s2mm_sts_tdata(s_axis_s2mm_sts_tdata),
        .s2mm_sts_tvalid(s_axis_s2mm_sts_tvalid),.s2mm_sts_tready(s_axis_s2mm_sts_tready),
        .s2mm_tdata(m_axis_s2mm_tdata),.s2mm_tvalid(m_axis_s2mm_tvalid),
        .s2mm_tready(m_axis_s2mm_tready),.s2mm_tlast(m_axis_s2mm_tlast)
    );
endmodule

// Time source: accepted transport beats at the SYS-clock FIFO output.
// This is NOT RF transmission or ADC sampling time. FIFO queuing is observable
// latency, not a reason to substitute configured frame periods for timestamps.
module radar_frame_metadata (
    input wire clk,rst_n,stream_valid,stream_ready,stream_last,pop,clear,
    output wire [31:0] status,
    output reg [31:0] next_sequence,
    output wire [31:0] head_sequence,head_bytes,
    output wire [63:0] head_first,head_last,current_ticks
);
    (* ram_style="distributed" *) reg [191:0] records[0:63];
    reg [5:0] rd_ptr,wr_ptr;
    reg [6:0] count;
    reg [63:0] ticks,first_tick;
    reg [31:0] bytes;
    reg active,overflow,clear_race;
    wire accepted=stream_valid && stream_ready;
    wire take=pop && count!=0;
    wire push=accepted && stream_last;
    wire store_record=push && (count<64 || take);
    assign status={overflow,active,clear_race,22'b0,count};
    assign current_ticks=ticks;
    assign {head_bytes,head_last,head_first,head_sequence}=count!=0 ? records[rd_ptr] : 192'b0;
    always @(posedge clk) begin
        if(!rst_n) begin
            ticks<=0;first_tick<=0;bytes<=0;active<=0;
            rd_ptr<=0;wr_ptr<=0;count<=0;next_sequence<=0;overflow<=0;clear_race<=0;
        end else begin
            ticks<=ticks+1'b1;
            if(clear && !accepted) begin
                rd_ptr<=0;wr_ptr<=0;count<=0;next_sequence<=0;
                overflow<=0;clear_race<=0;active<=0;bytes<=0;
            end else begin
                // A delayed CLEAR colliding with a beat never erases that beat.
                // Sticky evidence invalidates the software time/sequence claim.
                if(clear && accepted) clear_race<=1;
                if(take) rd_ptr<=rd_ptr+1'b1;
                if(store_record) begin
                    // Match the capture DMA's one-based transport packet IDs.
                    records[wr_ptr]<={bytes+32'd8,ticks,active ? first_tick : ticks,next_sequence+32'd1};
                    wr_ptr<=wr_ptr+1'b1;
                end
                case({store_record,take})
                    2'b10:count<=count+1'b1;
                    2'b01:count<=count-1'b1;
                    default:count<=count;
                endcase
                if(accepted) begin
                    if(!active) first_tick<=ticks;
                    if(stream_last) begin
                        next_sequence<=next_sequence+1'b1;active<=0;bytes<=0;
                        if(!store_record) overflow<=1;
                    end else begin active<=1;bytes<=bytes+32'd8;end
                end
            end
        end
    end
endmodule
