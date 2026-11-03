`timescale 1ns/1ps
`default_nettype none
// This is digital output qualification, NOT an analog power supervisor.
// After loss of a previously qualified supply, IO_ALLOW must be cleared
// explicitly before outputs may be enabled again. UART transactions must be
// idle before changing io_cmd, except emergency shutdown.
module radar_io_guard #(parameter integer POWER_QUAL_CYCLES=100000)(
    input wire clk, input wire resetn, input wire [3:0] io_cmd,
    input wire pg_raw, input wire nerr_n_raw, input wire uart_rx_raw,
    input wire ps_uart_tx,
    output wire ps_uart_rx, output wire tx_data, output wire tx_oe,
    output wire nrst_low, output wire sync_data, output wire sync_oe,
    output wire pg_sync, output wire nerr_n_sync,
    output reg power_qualified, output reg power_lost, output wire io_active
);
    wire rx_sync;
    reg [31:0] good_count;
    reg had_power;
    radar_sync_bit u_pg(.clk(clk),.arstn(resetn),.din(pg_raw),.dout(pg_sync));
    radar_sync_bit #(.INIT(1'b1)) u_nerr(.clk(clk),.arstn(resetn),.din(nerr_n_raw),.dout(nerr_n_sync));
    radar_sync_bit #(.INIT(1'b1)) u_uart_rx(.clk(clk),.arstn(resetn),.din(uart_rx_raw),.dout(rx_sync));
    always @(posedge clk or negedge resetn) begin
        if(!resetn) begin good_count<=0; power_qualified<=0; power_lost<=0; had_power<=0; end
        else begin
            if(!pg_sync) begin good_count<=0; power_qualified<=0; end
            else if(!power_qualified) begin
                if(good_count==POWER_QUAL_CYCLES-1) power_qualified<=1;
                else good_count<=good_count+1'b1;
            end
            if(!io_cmd[0]) begin power_lost<=0; had_power<=0; end
            else begin
                if(power_qualified) had_power<=1;
                if(had_power && !pg_sync) power_lost<=1;
            end
        end
    end
    // pg_raw also removes output enables without waiting for the 3-FF sampler.
    // This cannot guarantee immunity to fast supply collapse or back-powering.
    assign io_active = resetn && io_cmd[0] && power_qualified && !power_lost;
    assign nrst_low  = io_active && pg_raw && io_cmd[2];
    assign tx_oe     = io_active && pg_raw && io_cmd[1] && !io_cmd[2];
    assign tx_data   = ps_uart_tx;
    assign ps_uart_rx = (io_active && io_cmd[1] && !io_cmd[2]) ? rx_sync : 1'b1;
    assign sync_oe   = io_active && pg_raw && !io_cmd[2];
    assign sync_data = io_cmd[3] && sync_oe;
endmodule
`default_nettype wire
