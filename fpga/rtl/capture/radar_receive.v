`timescale 1ns/1ps
`default_nettype none
// RX-domain composition. Commands are atomically delivered from the mailbox.
// An ARM can be pending while BITCLK is absent. It is not "capturing" until
// real clocks, deserializer and FIFO reset release are ready. Acquisition
// starts only on a subsequent qualified FRCLK idle gap; pre-ready data are
// not claimed as captured. Firmware must supply an idle-clock preamble for
// lossless capture of its very first burst.
module radar_receive(
    input wire rx_clk, input wire common_resetn, input wire stream_resetn,
    input wire deser_resetn, input wire cmd_valid, input wire [143:0] cmd_data,
    input wire abort_level,
    input wire [7:0] fr_byte, input wire [7:0] data0_byte, input wire [7:0] data1_byte,
    input wire [31:0] fifo_count, input wire fifo_prog_full, input wire fifo_almost_full,
    output wire [63:0] tdata, output wire [7:0] tkeep,
    output wire tvalid, input wire tready, output wire tlast,
    output wire [383:0] snapshot, output reg complete, output reg fault,
    output wire first_seen, output wire pending, output reg idle_observed
);
    reg arm_pending, arm_pulse, stop_pulse, canceled;
    reg [4:0] format;
    reg [31:0] pairs_cfg, limit_cfg, skip_cfg, session_cfg;
    reg [3:0] warmup;
    reg decoder_enable;
    wire [31:0] pair_data;
    wire pair_valid, burst_first, framing_error, idle_gap;
    wire [3:0] phase;
    wire [31:0] decoder_words, decoder_errors;
    wire [2:0] state;
    wire [15:0] faults;
    wire [63:0] accepted_pairs, accepted_beats;
    wire [31:0] completed_packets, peak_fifo, current_session;
    wire prog_full_seen, almost_full_seen;
    wire abort_rx;
    radar_sync_bit u_abort(.clk(rx_clk),.arstn(common_resetn),.din(abort_level),.dout(abort_rx));
    assign pending=arm_pending;
    // These are CDC SOURCES. Register the state decode in RX; the destination
    // synchronizer cannot remove a physical combinational decode glitch.
    // Fault and successful/canceled terminal states persist until common reset.
    // Abort wins over completion. Idle is a qualified level, not a sticky event.
    always @(posedge rx_clk or negedge common_resetn) begin
        if(!common_resetn) begin complete<=0; fault<=0; idle_observed<=0; end
        else begin
            fault<=fault || (faults!=0);
            if(abort_rx || fault || faults!=0) complete<=0;
            else if(state==3'd4 || canceled) complete<=1;
            idle_observed<=idle_gap && decoder_enable && stream_resetn && deser_resetn;
        end
    end
    always @(posedge rx_clk or negedge common_resetn) begin
        if(!common_resetn) begin
            arm_pending<=0; arm_pulse<=0; stop_pulse<=0; canceled<=0; format<=0;
            pairs_cfg<=0; limit_cfg<=0; skip_cfg<=0; session_cfg<=0;
            warmup<=0; decoder_enable<=0;
        end else begin
            arm_pulse<=0; stop_pulse<=0;
            if(!stream_resetn || !deser_resetn) begin warmup<=0; decoder_enable<=0; end
            else if(!decoder_enable) begin
                if(warmup==7) decoder_enable<=1;
                else warmup<=warmup+1'b1;
            end
            if(cmd_valid) begin
                case(cmd_data[143:136])
                    8'h01: begin
                        pairs_cfg<=cmd_data[31:0]; limit_cfg<=cmd_data[63:32];
                        skip_cfg<=cmd_data[95:64]; session_cfg<=cmd_data[127:96];
                        format<=cmd_data[132:128]; arm_pending<=1; canceled<=0;
                        warmup<=0; decoder_enable<=0;
                    end
                    8'h02: begin
                        stop_pulse<=1;
                        // Pending ARM is canceled if STOP precedes activation.
                        if(arm_pending) begin arm_pending<=0; canceled<=1; end
                    end
                    default: begin end
                endcase
            // The official asynchronous FIFO may remain internally busy after
            // its external reset releases. Keep ARM pending until its write
            // interface is actually ready; capture still needs a later idle gap.
            end else if(arm_pending && decoder_enable && stream_resetn && tready && !abort_rx) begin
                arm_pulse<=1; arm_pending<=0;
            end
            if(abort_rx) arm_pending<=0;
        end
    end
    radar_fclk16_decode u_decode(
        .clk(rx_clk),.resetn(stream_resetn && decoder_enable),.cfg(format),
        .fr_bits(fr_byte),.d0_bits(data0_byte),.d1_bits(data1_byte),
        .pair_data(pair_data),.pair_valid(pair_valid),.burst_first(burst_first),
        .framing_error(framing_error),.idle_gap(idle_gap),.phase(phase),
        .words_seen(decoder_words),.errors_seen(decoder_errors));
    radar_raw_packetizer u_packetizer(
        .clk(rx_clk),.resetn(stream_resetn),.arm(arm_pulse),.stop(stop_pulse),
        // Command cancellation also reaches the packetizer while its stream
        // reset stays released, preserving any stalled AXIS beat and counters.
        .abort_capture(abort_rx || !deser_resetn || !common_resetn),
        .packet_pairs(pairs_cfg),.packet_limit(limit_cfg),.skip_pairs(skip_cfg),.session_id(session_cfg),
        .idle_gap(idle_gap && decoder_enable),.framing_error(framing_error),
        .pair_valid(pair_valid && decoder_enable),.pair_data(pair_data),
        .fifo_count(fifo_count),.fifo_prog_full(fifo_prog_full),.fifo_almost_full(fifo_almost_full),
        .m_tdata(tdata),.m_tkeep(tkeep),.m_tvalid(tvalid),.m_tready(tready),.m_tlast(tlast),
        .state(state),.faults(faults),.accepted_pairs(accepted_pairs),.accepted_beats(accepted_beats),
        .completed_packets(completed_packets),.peak_fifo(peak_fifo),.current_session(current_session),
        .first_data_seen(first_seen),.prog_full_seen(prog_full_seen),.almost_full_seen(almost_full_seen));
    // Twelve coherent 32-bit words. Word0 is the least significant word.
    assign snapshot = {
        fifo_count,
        {15'd0,canceled,format,phase,decoder_enable,arm_pending,burst_first,stream_resetn,deser_resetn,
         prog_full_seen,almost_full_seen},
        decoder_errors,decoder_words,
        {13'd0,state,faults}, current_session,peak_fifo,completed_packets,
        accepted_beats[63:32],accepted_beats[31:0],accepted_pairs[63:32],accepted_pairs[31:0]
    };
endmodule
`default_nettype wire
