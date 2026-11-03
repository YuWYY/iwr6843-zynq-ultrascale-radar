`timescale 1ns/1ps
// Experimental phase-independent temporal detector. ABI/mathematics are in
// verification/20261101_pl_dsp_01/CONTRACT.md. DDR header commit is transactional:
// invalidate before writing cells, commit only after all successful cell writes.
module radar_temporal_detect #(
    parameter integer CELL_COUNT = 8192
) (
    input  wire         clk, rst_n, cancel, start_frame,
    output wire         frame_ready,
    output reg          frame_header_valid,
    input  wire [511:0] frame_header_in,
    input  wire [63:0]  timestamp_us,
    input  wire [31:0]  config_epoch, acquisition_id, frame_index,
    input  wire [31:0]  validity_flags, alpha_q31, temporal_control,
    input  wire [31:0]  hold_threshold_q16,
    output reg  [511:0] frame_header_out,
    output reg  [31:0]  frame_flags,
    input  wire         cell_valid,
    output wire         cell_ready,
    input  wire [12:0]  cell_index,
    input  wire [63:0]  power,
    input  wire [511:0] state_in,
    input  wire         cell_local_max,
    output reg          result_valid,
    input  wire         result_ready,
    output reg  [12:0]  result_index,
    output reg  [511:0] state_out,
    output reg  [63:0]  enhanced_power,
    output reg          fresh_evidence,
    output reg  [31:0]  result_flags
);
    localparam [31:0] MAGIC=32'h544d5031;
    localparam [31:0] F_WARM=1, F_SPARSE=2, F_INVALID=4, F_IDENTITY=8,
        F_CONFIG=16, F_MANUAL=32, F_FALLBACK=64, F_INIT=128,
        F_SAT=256, F_ORDER=512, F_TIME_UNTRUSTED=1024;
    localparam [3:0] IDLE=0, CELL=1, DIVIDE=2, ROUND=3, OUTPUT=4,
        P_SUM=5, P_MEAN=6, P_REM=7, P_RNE=8, P_PREP=9;
    reg [3:0] machine;
    reg [13:0] expected_index;
    reg [63:0] time_reg;
    reg [31:0] alpha_reg, inverse_alpha_reg, hold_reg;
    reg [3:0] mask_reg;
    reg [1:0] slot_reg;
    reg init_reg, discard_reg;

    reg [511:0] next_header;
    reg [31:0] next_flags;
    reg [3:0] next_mask;
    reg [1:0] next_slot;
    reg next_init, next_discard, header_good;
    reg [7:0] old_warm;
    reg [63:0] old_time;
    integer h, population;

    // Frame-global eligibility is decided before accepting any cell.
    always @* begin
        next_header=0; next_flags=0; next_mask=0; next_slot=0;
        next_init=0; next_discard=0; population=0; old_warm=0;
        header_good=(frame_header_in[31:0]==MAGIC &&
                     frame_header_in[455:448]!=0 && frame_header_in[455:452]==0 &&
                     frame_header_in[463:456]<4 && frame_header_in[471:464]>=1 &&
                     frame_header_in[471:464]<=4 && frame_header_in[511:472]==0);
        old_time=frame_header_in[191:128];
        if ((validity_flags&32'd3)!=32'd3 || (validity_flags&32'd24)!=0 ||
            alpha_q31>32'h80000000 || (temporal_control&32'hfffffffe)!=0 ||
            hold_threshold_q16<32'd65536) next_flags=next_flags|F_INVALID;
        if (header_good && ((validity_flags[2] && timestamp_us<=old_time) ||
            acquisition_id<frame_header_in[95:64] ||
            (acquisition_id==frame_header_in[95:64] &&
             frame_index<=frame_header_in[127:96]))) next_flags=next_flags|F_IDENTITY;
        if (next_flags!=0) begin
            next_flags=next_flags|F_INIT;
            next_init=1; next_discard=1;
        end else if(!validity_flags[2]) begin
            next_flags=F_INIT|F_FALLBACK|F_TIME_UNTRUSTED;
            next_init=1;
        end else begin
            next_init=!header_good || temporal_control[0] ||
                      config_epoch!=frame_header_in[63:32];
            if (next_init) begin
                next_flags=next_flags|F_INIT;
                if (header_good && config_epoch!=frame_header_in[63:32])
                    next_flags=next_flags|F_CONFIG;
                if (temporal_control[0]) next_flags=next_flags|F_MANUAL;
            end else begin
                next_header=frame_header_in;
                next_slot=frame_header_in[457:456];
                old_warm=frame_header_in[471:464];
                for (h=0;h<4;h=h+1)
                    if (frame_header_in[448+h] &&
                        frame_header_in[192+64*h+:64]<=timestamp_us &&
                        timestamp_us-frame_header_in[192+64*h+:64]<=64'd500000)
                        next_mask[h]=1'b1;
            end
            next_mask[next_slot]=1'b1;
            for(h=0;h<4;h=h+1) population=population+next_mask[h];
            if (old_warm<4) next_flags=next_flags|F_WARM|F_FALLBACK;
            if (population<2) next_flags=next_flags|F_SPARSE|F_FALLBACK;
            next_header[31:0]=MAGIC;
            next_header[63:32]=config_epoch;
            next_header[95:64]=acquisition_id;
            next_header[127:96]=frame_index;
            next_header[191:128]=timestamp_us;
            next_header[192+64*next_slot+:64]=timestamp_us;
            next_header[455:448]={4'd0,next_mask};
            next_header[463:456]={6'd0,(next_slot+2'd1)};
            next_header[471:464]=(old_warm<4)?old_warm+8'd1:8'd4;
            next_header[511:472]=0;
        end
    end

    // Prototype: accept one cell, then read only its immutable input registers.
    reg [95:0] ema_old_product_reg, ema_new_product_reg, evidence_product_reg;
    reg [95:0] weighted_reg;
    reg [63:0] rounded_background_reg;
    reg evidence_reg;
    wire ema_round_up=(weighted_reg[30:0]>31'h40000000) ||
        (weighted_reg[30:0]==31'h40000000 && weighted_reg[31]);
    reg [511:0] state_reg;
    reg [63:0] power_reg;
    reg local_max_reg;
    reg [65:0] sum_reg, mean_floor_reg, mean_remainder_reg;
    reg [2:0] count_reg;
    reg [63:0] mean_power_reg;
    reg [65:0] mean_floor_comb;
    always @* begin
        case(count_reg)
            1: mean_floor_comb=sum_reg;
            2: mean_floor_comb=sum_reg>>1;
            3: mean_floor_comb=sum_reg/66'd3;
            4: mean_floor_comb=sum_reg>>2;
            default: mean_floor_comb=0;
        endcase
    end
    wire mean_round_up=((mean_remainder_reg<<1)>count_reg) ||
        (((mean_remainder_reg<<1)==count_reg) && mean_floor_reg[0]);

    reg [511:0] computed_state;
    reg [63:0] background, denominator, protect_until;
    reg [63:0] history [0:3];
    reg [65:0] sum_power;
    reg [63:0] rounded_background;
    reg evidence;
    integer c, eligible_count;
    always @* begin
        computed_state=0;
        background=init_reg?64'd0:state_reg[63:0];
        protect_until=init_reg?64'd0:state_reg[383:320];
        denominator=(background==0)?64'd1:background;
        sum_power=0; eligible_count=0;
        for(c=0;c<4;c=c+1) begin
            history[c]=init_reg?64'd0:state_reg[64+64*c+:64];
            if(c==slot_reg) history[c]=power_reg;
            if(mask_reg[c]) begin
                sum_power=sum_power+{2'd0,history[c]};
                eligible_count=eligible_count+1;
            end
            computed_state[64+64*c+:64]=history[c];
        end
        evidence=evidence_reg;
        rounded_background=rounded_background_reg;
        if(init_reg) begin
            background=power_reg; protect_until=0;
        end else if((frame_flags&F_WARM)!=0) begin
            background=rounded_background; protect_until=0;
        end else begin
            if(evidence) protect_until=(time_reg>64'hfffffffffff85edf)?
                64'hffffffffffffffff:time_reg+64'd500000;
            if(protect_until<=time_reg) begin
                background=rounded_background; protect_until=0;
            end
        end
        computed_state[63:0]=background;
        computed_state[383:320]=protect_until;
    end

    reg [95:0] dividend, quotient;
    reg [64:0] remainder;
    reg [63:0] divisor;
    reg [6:0] divider_count;
    wire [64:0] shifted_remainder={remainder[63:0],dividend[95]};
    wire quotient_bit=shifted_remainder>={1'b0,divisor};
    wire [95:0] rounded_quotient=quotient+
        (((remainder<<1)>{1'b0,divisor} ||
          ((remainder<<1)=={1'b0,divisor} && quotient[0]))?96'd1:96'd0);
    assign frame_ready=(machine==IDLE);
    assign cell_ready=(machine==CELL);

    always @(posedge clk) begin
        if(!rst_n || cancel) begin
            machine<=IDLE; expected_index<=0; frame_header_valid<=0;
            frame_header_out<=0; frame_flags<=0;
            result_valid<=0; result_index<=0; state_out<=0;
            enhanced_power<=0; fresh_evidence<=0; result_flags<=0;
            time_reg<=0; alpha_reg<=0; inverse_alpha_reg<=0; hold_reg<=0; mask_reg<=0;
            slot_reg<=0; init_reg<=0; discard_reg<=0;
            ema_old_product_reg<=0; ema_new_product_reg<=0; evidence_product_reg<=0;
            weighted_reg<=0; rounded_background_reg<=0; evidence_reg<=0;
            state_reg<=0; power_reg<=0; local_max_reg<=0;
            sum_reg<=0; count_reg<=0; mean_floor_reg<=0; mean_remainder_reg<=0; mean_power_reg<=0;
            dividend<=0; quotient<=0; remainder<=0; divisor<=1; divider_count<=0;
        end else begin
            frame_header_valid<=0;
            case(machine)
                IDLE: if(start_frame) begin
                    frame_header_out<=next_header; frame_flags<=next_flags;
                    frame_header_valid<=1; expected_index<=0;
                    time_reg<=timestamp_us; alpha_reg<=alpha_q31;
                    inverse_alpha_reg<=32'h80000000-alpha_q31;
                    hold_reg<=hold_threshold_q16; mask_reg<=next_mask;
                    slot_reg<=next_slot; init_reg<=next_init; discard_reg<=next_discard;
                    machine<=CELL;
                end
                CELL: if(cell_valid) begin
                    result_index<=cell_index; result_flags<=frame_flags;
                    fresh_evidence<=0; enhanced_power<=0;
                    if(cell_index!=expected_index[12:0] || expected_index>=CELL_COUNT) begin
                        state_out<=0; result_flags<=frame_flags|F_ORDER;
                        frame_header_out<=0; frame_flags<=frame_flags|F_ORDER;
                        discard_reg<=1; result_valid<=1; machine<=OUTPUT;
                    end else if(discard_reg) begin
                        state_out<=0; result_valid<=1; machine<=OUTPUT;
                    end else if((frame_flags&F_TIME_UNTRUSTED)!=0) begin
                        state_out<=0; enhanced_power<=power;
                        result_valid<=1; machine<=OUTPUT;
                    end else begin
                        state_reg<=state_in; power_reg<=power;
                        local_max_reg<=cell_local_max;
                        machine<=P_SUM;
                    end
                end
                P_SUM: begin
                    sum_reg<=sum_power; count_reg<=eligible_count;
                    ema_old_product_reg<=(init_reg?64'd0:state_reg[63:0])*inverse_alpha_reg;
                    ema_new_product_reg<=power_reg*alpha_reg;
                    evidence_product_reg<=denominator*hold_reg;
                    divisor<=denominator;
                    machine<=P_MEAN;
                end
                P_MEAN: begin
                    weighted_reg<=ema_old_product_reg+ema_new_product_reg;
                    evidence_reg<=local_max_reg && power_reg!=0 &&
                        {16'd0,power_reg,16'd0}>=evidence_product_reg;
                    mean_floor_reg<=mean_floor_comb;
                    machine<=P_REM;
                end
                P_REM: begin
                    rounded_background_reg<=weighted_reg[94:31]+{{63{1'b0}},ema_round_up};
                    case(count_reg)
                        1: mean_remainder_reg<=0;
                        2: mean_remainder_reg<=sum_reg&66'd1;
                        3: mean_remainder_reg<=sum_reg-(mean_floor_reg+{mean_floor_reg[64:0],1'b0});
                        4: mean_remainder_reg<=sum_reg&66'd3;
                        default: mean_remainder_reg<=0;
                    endcase
                    machine<=P_RNE;
                end
                P_RNE: begin
                    state_out<=computed_state; fresh_evidence<=evidence;
                    mean_power_reg<=mean_floor_reg[63:0]+{{63{1'b0}},mean_round_up};
                    machine<=P_PREP;
                end
                P_PREP: begin
                    if((frame_flags&F_FALLBACK)!=0) begin
                        enhanced_power<=power_reg; result_valid<=1; machine<=OUTPUT;
                    end else begin
                        dividend<={mean_power_reg,32'd0};
                        quotient<=0; remainder<=0; divider_count<=0;
                        machine<=DIVIDE;
                    end
                end
                DIVIDE: begin
                    dividend<=dividend<<1;
                    quotient<={quotient[94:0],quotient_bit};
                    remainder<=quotient_bit?shifted_remainder-{1'b0,divisor}:shifted_remainder;
                    if(divider_count==95) machine<=ROUND;
                    else divider_count<=divider_count+1;
                end
                ROUND: begin
                    if(rounded_quotient[95:64]!=0) begin
                        enhanced_power<=64'hffffffffffffffff;
                        result_flags<=result_flags|F_SAT;
                    end else enhanced_power<=rounded_quotient[63:0];
                    result_valid<=1; machine<=OUTPUT;
                end
                OUTPUT: if(result_ready) begin
                    result_valid<=0;
                    if(expected_index==CELL_COUNT-1 || (result_flags&F_ORDER)!=0)
                        machine<=IDLE;
                    else begin expected_index<=expected_index+1; machine<=CELL; end
                end
                default: machine<=IDLE;
            endcase
        end
    end
endmodule
