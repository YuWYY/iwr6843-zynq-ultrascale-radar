`timescale 1ns/1ps
`default_nettype none
// RX control-domain executor. All durations below are real RX75 cycles.
// Reset is a persistent epoch cancellation, shared with request/result CDC.
module radar_phy_delay(
    input wire clk_rx, input wire resetn_rx, input wire observation_resetn, input wire cal_ready,
    input wire cfg_valid, input wire [26:0] cfg_target,
    input wire [26:0] tap_count_live, input wire report_busy,
    output reg en_vtc, output reg [2:0] load,
    output reg [26:0] tap_values, output reg ready, output reg executing,
    output reg baseline_valid, output reg [26:0] baseline,
    output reg [26:0] readback, output reg [31:0] observation_generation,
    output reg report_send, output reg [58:0] report_data,
    output reg [3:0] state
);
    localparam WAIT_CAL=0, VTC_OFF=1, SAMPLE=2, VERIFY=3, PLAN=4,
               SETUP=5, LOAD=6, WAIT_READ=7, VERIFY_STEP=8,
               FINISH_LOW=9, RESTORE=10, READY=11, FAILED=12;
    localparam SUCCESS=4'd1, BELOW_BASELINE=4'd3, READBACK_INVALID=4'd4,
               NO_CONVERGENCE=4'd5, RDY_LOST=4'd6;
    reg [7:0] timer, rounds;
    reg [26:0] target, current, sample;
    reg transaction, report_pending;
    reg [2:0] next_mask, load_mask;
    reg [26:0] next_values;
    integer n, difference, next_tap;
    // Measurement identity survives local cancellation / RECAL epochs. Only
    // the global RX reset clears it; stopped clocks never advance it.
    always @(posedge clk_rx or negedge observation_resetn) begin
        if(!observation_resetn) observation_generation<=0;
        else if(resetn_rx && cal_ready && state==RESTORE && timer==15)
            observation_generation<=observation_generation+1'b1;
    end
    always @* begin
        next_values=current; next_mask=0;
        for(n=0;n<3;n=n+1) begin
            difference=$signed({1'b0,target[n*9 +: 9]})-
                       $signed({1'b0,current[n*9 +: 9]});
            if(difference>8) difference=8;
            else if(difference < -8) difference=-8;
            next_tap={1'b0,current[n*9 +: 9]}+difference;
            next_values[n*9 +: 9]=next_tap[8:0];
            next_mask[n]=(difference!=0);
        end
    end
    task report;
        input [3:0] result;
        begin
            report_data<={transaction,result,baseline,readback};
            report_pending<=1;
        end
    endtask
    task fail;
        input [3:0] result;
        begin
            en_vtc<=1; load<=0; ready<=0; executing<=0;
            baseline_valid<=0; state<=FAILED; report(result);
        end
    endtask
    always @(posedge clk_rx or negedge resetn_rx) begin
        if(!resetn_rx) begin
            en_vtc<=1; load<=0; tap_values<=0; ready<=0; executing<=0;
            baseline_valid<=0; baseline<=0; readback<=0;
            report_send<=0; report_data<=0;
            state<=WAIT_CAL; timer<=0; rounds<=0; target<=0; current<=0;
            sample<=0; transaction<=0; report_pending<=0; load_mask<=0;
        end else begin
            report_send<=0; load<=0;
            if(report_pending && !report_busy) begin
                report_send<=1; report_pending<=0;
            end
            case(state)
                WAIT_CAL: if(cal_ready) begin
                    en_vtc<=0; timer<=0; transaction<=0; state<=VTC_OFF;
                end
                VTC_OFF: begin
                    if(timer==11) begin timer<=0; state<=SAMPLE; end
                    else timer<=timer+1'b1;
                end
                SAMPLE: begin sample<=tap_count_live; state<=VERIFY; end
                VERIFY: begin
                    // Equality rejects X-containing samples in simulation;
                    // the synthesized circuit checks two real observations.
                    if(tap_count_live==sample) begin
                        current<=tap_count_live; readback<=tap_count_live;
                        if(!transaction) begin
                            baseline<=tap_count_live; baseline_valid<=1;
                            timer<=0; state<=FINISH_LOW;
                        end else state<=PLAN;
                    end else fail(READBACK_INVALID);
                end
                PLAN: begin
                    if(current==target) begin timer<=0; state<=FINISH_LOW; end
                    else if(rounds==64) fail(NO_CONVERGENCE);
                    else begin
                        tap_values<=next_values; load_mask<=next_mask;
                        rounds<=rounds+1'b1; timer<=0; state<=SETUP;
                    end
                end
                SETUP: begin
                    // Two full clocks of CNTVALUEIN setup, then one LOAD.
                    if(timer==1) begin load<=load_mask; state<=LOAD; timer<=0; end
                    else timer<=timer+1'b1;
                end
                LOAD: begin timer<=0; state<=WAIT_READ; end
                WAIT_READ: begin
                    if(timer==11) begin sample<=tap_count_live; state<=VERIFY_STEP; end
                    else timer<=timer+1'b1;
                end
                VERIFY_STEP: begin
                    if(tap_count_live==sample) begin
                        if(tap_count_live==tap_values) begin
                            current<=tap_count_live; readback<=tap_count_live;
                            state<=PLAN;
                        end else fail(NO_CONVERGENCE);
                    end else fail(READBACK_INVALID);
                end
                FINISH_LOW: begin
                    if(timer==11) begin en_vtc<=1; timer<=0; state<=RESTORE; end
                    else timer<=timer+1'b1;
                end
                RESTORE: begin
                    if(timer==15) begin
                        ready<=1; executing<=0; state<=READY;
                        report(SUCCESS);
                    end else timer<=timer+1'b1;
                end
                READY: if(cfg_valid) begin
                    transaction<=1;
                    if(cfg_target[8:0]<baseline[8:0] ||
                       cfg_target[17:9]<baseline[17:9] ||
                       cfg_target[26:18]<baseline[26:18]) begin
                        // Rejection preserves the previously valid setting.
                        report_data<={1'b1,BELOW_BASELINE,baseline,readback};
                        report_pending<=1;
                    end else begin
                        target<=cfg_target; ready<=0; executing<=1;
                        en_vtc<=0; timer<=0; rounds<=0; state<=VTC_OFF;
                    end
                end
                FAILED: begin en_vtc<=1; ready<=0; executing<=0; end
                default: fail(NO_CONVERGENCE);
            endcase
            if(!cal_ready && state!=WAIT_CAL && state!=FAILED) fail(RDY_LOST);
        end
    end
endmodule
`default_nettype wire
