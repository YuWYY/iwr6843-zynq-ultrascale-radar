`timescale 1ns/1ps
`default_nettype none
// Unique dual-32-bit AXI GPIO mailbox. All software accesses remain SYS clocked.
// ctrl[31]=toggle, [30:24]=opcode, [23:16]=index, [15:0]=argument.
// response[31]=ack toggle,[30]=error,[29:24]=result,[23:16]=index,[15:0]=value.
// ARM acknowledges QUEUING, not DMA completion or a valid radar frame.
module radar_control #(
    parameter integer SYS_CLOCK_HZ=100000000,
    parameter integer RX_CLOCK_TIMEOUT=1000000,
    parameter integer FIRST_DATA_TIMEOUT=500000000
)(
    input wire clk, input wire resetn, input wire [31:0] ctrl_i,
    output reg [31:0] status_o, output reg [3:0] io_cmd,
    input wire pg, input wire pg_qualified, input wire nerr_n, input wire power_lost,
    input wire io_active, input wire tx_oe, input wire nrst_low, input wire sync_oe,
    input wire arm_admissible_sys, input wire ref_cal_ready_sys, input wire rx_ready_sys,
    input wire baseline_valid_sys, input wire [26:0] baseline_data,
    input wire [31:0] baseline_generation_sys,
    input wire [3:0] cfg_result, input wire [2:0] cfg_phase,
    input wire [3:0] phy_fault_sys,
    input wire [31:0] cfg_generation,
    input wire deser_ready, input wire fifo_ready,
    input wire dma_stream_resetn, input wire rx_heartbeat,
    input wire rx_complete, input wire rx_fault, input wire rx_first, input wire rx_pending,
    input wire rx_idle,
    input wire cmd_busy, output reg cmd_send, output reg [143:0] cmd_data,
    input wire snapshot_busy, input wire snapshot_valid, input wire [383:0] snapshot_data,
    input wire [31:0] snapshot_generation, output reg snapshot_request,
    input wire tap_snapshot_valid, input wire [63:0] tap_snapshot_data,
    input wire [31:0] tap_snapshot_generation,
    input wire delay_busy, output reg delay_send, output reg [27:0] delay_data,
    output reg recover_active, output reg abort_level, output reg transaction_cancel,
    output reg session_open, output reg [15:0] sys_faults
);
    localparam OP_SAFE=7'h00, OP_IO=7'h01, OP_CFG_LO=7'h02, OP_CFG_HI=7'h03,
               OP_READ=7'h04, OP_ARM=7'h05, OP_STOP=7'h06, OP_ABORT=7'h07,
               OP_RECOVER=7'h08, OP_SNAPSHOT=7'h09, OP_DELAY=7'h0a, OP_RECAL=7'h0b;
    localparam OK=6'd0, BAD_OP=6'd1, BUSY=6'd2, BAD_INDEX=6'd3, BAD_VALUE=6'd4,
               NOT_READY=6'd5, BAD_CONTRACT=6'd6, NEED_RECOVER=6'd7, BAD_TOKEN=6'd8;
    reg seen_toggle;
    reg [31:0] shadow[0:7];
    reg [7:0] recover_count;
    reg [63:0] ticks, first_ticks, complete_ticks;
    reg [31:0] arm_age, clock_age;
    reg heartbeat_previous, clock_alive, session_saw_clock, first_recorded, end_recorded;
    reg [31:0] selected_value;
    reg selected_valid;
    reg [15:0] faults_next;
    integer k;
    wire new_cmd=ctrl_i[31]!=seen_toggle;
    wire [6:0] opcode=ctrl_i[30:24];
    wire [7:0] index=ctrl_i[23:16];
    wire [15:0] argument=ctrl_i[15:0];
    wire [31:0] flags={8'd0,sync_oe,nrst_low,tx_oe,dma_stream_resetn,
        (delay_busy || delay_send),rx_idle,rx_pending,recover_active,
        snapshot_valid,(snapshot_busy || snapshot_request),(cmd_busy || cmd_send),rx_fault,
        rx_complete,rx_first,session_open,fifo_ready,
        clock_alive,deser_ready,rx_ready_sys,power_lost,
        nerr_n,pg_qualified,pg,io_active};
    always @* begin
        selected_valid=1;
        case(index)
            8'h00: selected_value=32'h52430101; // RC v1.0.1: separate ARM/RX readiness and bounded PHY transactions.
            8'h01: selected_value=flags;
            8'h02: selected_value={16'd0,sys_faults};
            8'h03: selected_value=shadow[3];
            8'h04: selected_value=clock_age;
            8'h05: selected_value=first_ticks[31:0];
            8'h06: selected_value=first_ticks[63:32];
            8'h07: selected_value=complete_ticks[31:0];
            8'h08: selected_value=complete_ticks[63:32];
            8'h09: selected_value=snapshot_generation;
            8'h0a: selected_value=arm_age;
            8'h0b: begin selected_value=tap_snapshot_data[31:0]; selected_valid=tap_snapshot_valid; end
            8'h0c: begin selected_value=tap_snapshot_data[63:32]; selected_valid=tap_snapshot_valid; end
            8'h0d: selected_value=SYS_CLOCK_HZ;
            8'h0e: selected_value={28'd0,io_cmd};
            8'h0f: selected_value={21'd0,arm_admissible_sys,rx_ready_sys,ref_cal_ready_sys,
                baseline_valid_sys,cfg_phase,cfg_result};
            8'h10: selected_value=cfg_generation;
            8'h11: begin selected_value={5'd0,baseline_data}; selected_valid=baseline_valid_sys; end
            8'h12: selected_value=baseline_generation_sys;
            8'h13: selected_value=tap_snapshot_generation;
            8'h14: selected_value={28'd0,phy_fault_sys};
            default: begin
                selected_value=0;
                if(index>=8'h20 && index<=8'h2b) begin
                    selected_value=snapshot_data[(index-8'h20)*32 +: 32];
                    selected_valid=snapshot_valid;
                end else if(index>=8'h40 && index<=8'h47)
                    selected_value=shadow[index-8'h40];
                else selected_valid=0;
            end
        endcase
    end
    // One next-state owner for sticky faults and the SOURCE-REGISTERED abort.
    // A same-cycle fault/cancel wins over recording a successful completion.
    always @* begin
        faults_next=sys_faults;
        if(session_open && !end_recorded) begin
            if(!pg || power_lost) faults_next[0]=1;
            if(!nerr_n) faults_next[1]=1;
            // RX can legitimately be absent while an ARM waits for the radar.
            if(!ref_cal_ready_sys || (first_recorded && !rx_ready_sys)) faults_next[2]=1;
            if(session_saw_clock && !clock_alive) faults_next[3]=1;
            if(!dma_stream_resetn) faults_next[4]=1;
            if(!rx_first && arm_age>=FIRST_DATA_TIMEOUT-1) faults_next[5]=1;
            if(rx_fault) faults_next[7]=1;
        end
        if(new_cmd) begin
            if(opcode==OP_SAFE && session_open && !end_recorded) faults_next[8]=1;
            if(opcode==OP_ABORT && session_open) faults_next[8]=1;
            if(opcode==OP_IO && argument[15:4]==0 && session_open && !end_recorded &&
               (!argument[0] || argument[2])) faults_next[6]=1;
            if(opcode==OP_RECOVER && argument==16'hc1ea) faults_next=0;
        end
    end
    always @(posedge clk or negedge resetn) begin
        if(!resetn) begin
            seen_toggle<=0; status_o<=0; io_cmd<=0; recover_count<=0;
            recover_active<=0; abort_level<=0; transaction_cancel<=0;
            cmd_send<=0; cmd_data<=0; snapshot_request<=0; delay_send<=0; delay_data<=0;
            session_open<=0; sys_faults<=0; ticks<=0; first_ticks<=0; complete_ticks<=0;
            arm_age<=0; clock_age<=32'hffffffff; heartbeat_previous<=0; clock_alive<=0;
            session_saw_clock<=0; first_recorded<=0; end_recorded<=0;
            for(k=0;k<8;k=k+1) shadow[k]<=0;
            shadow[0]<=32768; shadow[1]<=1; shadow[3]<=1;
            // shadow[4] stays zero. Explicit RAW16 contract confirmation is mandatory.
        end else begin
            ticks<=ticks+1'b1;
            sys_faults<=faults_next; abort_level<=(faults_next!=0);
            transaction_cancel<=0;
            cmd_send<=0; snapshot_request<=0; delay_send<=0;
            if(recover_count!=0) begin
                recover_count<=recover_count-1'b1;
                if(recover_count==1) recover_active<=0;
            end
            heartbeat_previous<=rx_heartbeat;
            if(recover_active || !dma_stream_resetn) begin clock_age<=32'hffffffff; clock_alive<=0; end
            // The first accepted RX pair is also proof of clock activity. It
            // can precede bit7's first heartbeat after an initially stopped RX
            // clock; without this baseline an early partial burst never times out.
            else if(rx_heartbeat!=heartbeat_previous ||
                    (session_open && rx_first && !first_recorded)) begin clock_age<=0; clock_alive<=1; end
            else begin
                if(clock_age!=32'hffffffff) clock_age<=clock_age+1'b1;
                if(clock_age>=RX_CLOCK_TIMEOUT-1) clock_alive<=0;
            end
            if(session_open && !rx_complete) begin
                if(arm_age!=32'hffffffff) arm_age<=arm_age+1'b1;
                if(clock_alive) session_saw_clock<=1;
            end
            if(session_open && rx_first && !first_recorded) begin
                first_ticks<=ticks; first_recorded<=1;
            end
            if(session_open && rx_complete && !end_recorded && !rx_fault && faults_next==0 &&
               !recover_active && !(new_cmd && (opcode==OP_SAFE || opcode==OP_ABORT || opcode==OP_RECOVER))) begin
                complete_ticks<=ticks; end_recorded<=1;
            end
            if(new_cmd) begin
                seen_toggle<=ctrl_i[31];
                status_o<={ctrl_i[31],1'b0,OK,index,argument};
                case(opcode)
                    OP_SAFE: begin
                        io_cmd<=0; transaction_cancel<=1;
                    end
                    OP_IO: begin
                        if(argument[15:4]!=0) status_o<={ctrl_i[31],1'b1,BAD_VALUE,index,argument};
                        else begin
                            io_cmd<=argument[3:0];
                        end
                    end
                    OP_CFG_LO,OP_CFG_HI: begin
                        if(session_open || cmd_busy || cmd_send || recover_active)
                            status_o<={ctrl_i[31],1'b1,BUSY,index,argument};
                        else if(index>7) status_o<={ctrl_i[31],1'b1,BAD_INDEX,index,argument};
                        else if(opcode==OP_CFG_LO) shadow[index][15:0]<=argument;
                        else shadow[index][31:16]<=argument;
                    end
                    OP_READ: begin
                        if(argument>1) status_o<={ctrl_i[31],1'b1,BAD_VALUE,index,argument};
                        else if(!selected_valid) status_o<={ctrl_i[31],1'b1,NOT_READY,index,16'd0};
                        else status_o<={ctrl_i[31],1'b0,OK,index,
                            (argument[0] ? selected_value[31:16] : selected_value[15:0])};
                    end
                    OP_ARM: begin
                        if(session_open || sys_faults!=0)
                            status_o<={ctrl_i[31],1'b1,NEED_RECOVER,index,16'd0};
                        else if(cmd_busy || cmd_send || recover_active || delay_busy || delay_send)
                            status_o<={ctrl_i[31],1'b1,BUSY,index,16'd0};
                        else if(shadow[4][31:16]!=16'h1616 || shadow[4][15:5]!=0)
                            status_o<={ctrl_i[31],1'b1,BAD_CONTRACT,index,16'd0};
                        else if(shadow[0]<2 || shadow[0][0] || shadow[0]>2097150)
                            status_o<={ctrl_i[31],1'b1,BAD_VALUE,index,16'd0};
                        else if(!io_active || !nerr_n || !arm_admissible_sys || !dma_stream_resetn || nrst_low)
                            status_o<={ctrl_i[31],1'b1,NOT_READY,index,16'd0};
                        else begin
                            cmd_data<={8'h01,shadow[4][7:0],shadow[3],shadow[2],shadow[1],shadow[0]};
                            cmd_send<=1; session_open<=1; arm_age<=0; session_saw_clock<=clock_alive;
                            first_ticks<=0; complete_ticks<=0; first_recorded<=0; end_recorded<=0;
                        end
                    end
                    OP_STOP: begin
                        if(!session_open || sys_faults!=0)
                            status_o<={ctrl_i[31],1'b1,NOT_READY,index,16'd0};
                        else if(cmd_busy || cmd_send || recover_active)
                            status_o<={ctrl_i[31],1'b1,BUSY,index,16'd0};
                        else begin cmd_data<={8'h02,136'd0}; cmd_send<=1; end
                    end
                    OP_ABORT: begin transaction_cancel<=1; end
                    OP_RECOVER: begin
                        if(argument!=16'hc1ea)
                            status_o<={ctrl_i[31],1'b1,BAD_TOKEN,index,argument};
                        else begin
                            // Token is a SOFTWARE assertion: sensor stopped and DMA quiesced/reset.
                            // We cannot read DMA outstanding transactions through an AXI GPIO.
                            recover_count<=128; recover_active<=1; transaction_cancel<=1;
                            session_open<=0; arm_age<=0;
                            session_saw_clock<=0; first_recorded<=0; end_recorded<=0;
                            first_ticks<=0; complete_ticks<=0;
                            cmd_send<=0; snapshot_request<=0;
                        end
                    end
                    OP_SNAPSHOT: begin
                        if(snapshot_busy || snapshot_request || delay_busy || delay_send ||
                           recover_active || !dma_stream_resetn)
                            status_o<={ctrl_i[31],1'b1,BUSY,index,16'd0};
                        else if(!rx_ready_sys || !baseline_valid_sys)
                            status_o<={ctrl_i[31],1'b1,NOT_READY,index,16'd0};
                        else snapshot_request<=1;
                    end
                    OP_DELAY: begin
                        if(session_open || delay_busy || delay_send || snapshot_busy || snapshot_request || recover_active)
                            status_o<={ctrl_i[31],1'b1,BUSY,index,16'd0};
                        else if(!rx_ready_sys || !baseline_valid_sys)
                            status_o<={ctrl_i[31],1'b1,NOT_READY,index,16'd0};
                        else if((shadow[5]|shadow[6]|shadow[7])>511)
                            status_o<={ctrl_i[31],1'b1,BAD_VALUE,index,16'd0};
                        else begin
                            delay_data<={1'b0,shadow[7][8:0],shadow[6][8:0],shadow[5][8:0]}; delay_send<=1;
                        end
                    end
                    OP_RECAL: begin
                        if(session_open || delay_busy || delay_send || snapshot_busy || snapshot_request || recover_active)
                            status_o<={ctrl_i[31],1'b1,BUSY,index,16'd0};
                        else begin delay_data<={1'b1,27'd0}; delay_send<=1; end
                    end
                    default: status_o<={ctrl_i[31],1'b1,BAD_OP,index,argument};
                endcase
            end
        end
    end
endmodule
`default_nettype wire
