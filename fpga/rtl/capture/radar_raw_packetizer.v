`timescale 1ns/1ps
`default_nettype none
// The raw link cannot be backpressured. One complete AXIS beat and one half-beat
// can wait locally; the existing AMD asynchronous FIFO is the bulk buffer.
// Packet size is a positive EVEN number of 32-bit lane pairs (multiple of 8B).
// TLAST means a configured raw transport chunk, NEVER implicitly a radar frame.
// Fault: retain any already-present TVALID beat until handshake/reset, do not
// overwrite it and do not invent a successful truncated TLAST.
module radar_raw_packetizer(
    input wire clk, input wire resetn,
    input wire arm, input wire stop, input wire abort_capture,
    input wire [31:0] packet_pairs, input wire [31:0] packet_limit,
    input wire [31:0] skip_pairs, input wire [31:0] session_id,
    input wire idle_gap, input wire framing_error,
    input wire pair_valid, input wire [31:0] pair_data,
    input wire [31:0] fifo_count, input wire fifo_prog_full, input wire fifo_almost_full,
    output reg [63:0] m_tdata, output wire [7:0] m_tkeep,
    output reg m_tvalid, input wire m_tready, output reg m_tlast,
    output reg [2:0] state, output reg [15:0] faults,
    output reg [63:0] accepted_pairs, output reg [63:0] accepted_beats,
    output reg [31:0] completed_packets, output reg [31:0] peak_fifo,
    output reg [31:0] current_session, output reg first_data_seen,
    output reg prog_full_seen, output reg almost_full_seen
);
    localparam IDLE=3'd0, WAIT_GAP=3'd1, ACTIVE=3'd2, DRAIN=3'd3,
               COMPLETE=3'd4, FAULT=3'd5;
    reg [31:0] length_latched, limit_latched, skip_left, pair_index, packets_generated;
    reg half_full, stop_pending;
    reg [31:0] half_word;
    wire out_space = !m_tvalid || m_tready;
    wire last_pair = pair_index == length_latched-1'b1;
    assign m_tkeep=8'hff;
    always @(posedge clk or negedge resetn) begin
        if (!resetn) begin
            m_tdata<=0; m_tvalid<=0; m_tlast<=0; state<=IDLE; faults<=0;
            accepted_pairs<=0; accepted_beats<=0; completed_packets<=0;
            peak_fifo<=0; current_session<=0; first_data_seen<=0;
            prog_full_seen<=0; almost_full_seen<=0; length_latched<=0;
            limit_latched<=0; skip_left<=0; pair_index<=0; packets_generated<=0;
            half_full<=0; half_word<=0; stop_pending<=0;
        end else begin
            if (fifo_count>peak_fifo) peak_fifo<=fifo_count;
            if (fifo_prog_full) prog_full_seen<=1;
            if (fifo_almost_full) almost_full_seen<=1;
            if (m_tvalid && m_tready) begin
                m_tvalid<=0; accepted_beats<=accepted_beats+1'b1;
                if (m_tlast) begin
                    completed_packets<=completed_packets+1'b1;
                    if (state==DRAIN) state<=COMPLETE;
                end
            end
            if (stop) stop_pending<=1;
            if (arm) begin
                if (state!=IDLE || m_tvalid || half_full) begin
                    faults[3]<=1; state<=FAULT;
                end else if (packet_pairs<2 || packet_pairs[0] || packet_pairs>2097150) begin
                    faults[4]<=1; state<=FAULT;
                end else begin
                    length_latched<=packet_pairs; limit_latched<=packet_limit;
                    skip_left<=skip_pairs; current_session<=session_id;
                    state<=idle_gap ? ACTIVE : WAIT_GAP;
                end
            end
            if (state==WAIT_GAP && idle_gap) state<=ACTIVE;
            if (state==WAIT_GAP && (stop || stop_pending)) state<=COMPLETE;
            if ((state==ACTIVE || state==WAIT_GAP || state==DRAIN) && abort_capture) begin
                faults[2]<=1; state<=FAULT;
            end else if (state==ACTIVE && (stop || stop_pending) && pair_index==0 && !half_full) begin
                // At a chunk boundary there is no reason to wait for more radar data.
                state <= (m_tvalid && !m_tready) ? DRAIN : COMPLETE;
            end else if (state==ACTIVE && framing_error) begin
                faults[1]<=1; state<=FAULT;
            end else if (state==ACTIVE && pair_valid) begin
                if (skip_left!=0) skip_left<=skip_left-1'b1;
                else if (!half_full) begin
                    half_word<=pair_data; half_full<=1;
                    accepted_pairs<=accepted_pairs+1'b1;
                    first_data_seen<=1; pair_index<=pair_index+1'b1;
                end else if (!out_space) begin
                    faults[0]<=1; state<=FAULT;
                end else begin
                    m_tdata<={pair_data,half_word}; m_tvalid<=1; m_tlast<=last_pair;
                    half_full<=0; accepted_pairs<=accepted_pairs+1'b1;
                    if (last_pair) begin
                        pair_index<=0; packets_generated<=packets_generated+1'b1;
                        if (stop || stop_pending ||
                            (limit_latched!=0 && packets_generated==limit_latched-1'b1)) state<=DRAIN;
                    end else pair_index<=pair_index+1'b1;
                end
            end
        end
    end
endmodule
`default_nettype wire
