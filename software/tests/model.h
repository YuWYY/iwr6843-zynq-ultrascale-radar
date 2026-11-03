/* HOST behavioral model, not RTL/PHY/AXI timing simulation. */
#ifndef RADAR_TEST_MODEL_H
#define RADAR_TEST_MODEL_H
#include "radar_session.h"
#define MODEL_LOG_BYTES (20U*1024U*1024U)
typedef struct {
    u64 now,halt_at,reset_at,mailbox_at,phy_at,snapshot_at,arm_at,stream_at;
    u32 halt_latency,reset_latency,mailbox_latency,phy_latency,snapshot_latency,arm_latency;
    u32 dma[64],pages[256],reply,pending_reply;
    u32 dma_reset_writes,dma_halt_writes,gpio_writes,ops[128],cache_flushes,cache_invalidates;
    u32 starts,stops,resets,config_commands,produced_bytes,producer_resets;
    int halt_stuck,reset_stuck,mailbox_stuck,phy_stuck,snapshot_stuck,arm_stuck;
    int no_rx_clock,no_payload,wrong_snapshot_session,wrong_snapshot_count,corrupt_bd,short_bd;
    int uart_bad_version,uart_echo_only,uart_error,uart_silent,uart_start_fail,uart_stop_fail;
    int uart_start_ignored;
    int source_running,mapping_fail;
    /* READ-only diagnostic fault seams; zero leaves the model unchanged. */
    u8 read_reject_page,read_reject_code,read_change_page;
    u32 read_change_after,read_change_count;
    char uart_tx[1024],uart_rx[8192],last_command[256];
    u32 uart_tx_count,uart_rx_head,uart_rx_tail;
    char log[MODEL_LOG_BYTES];u32 log_count;
    u32 command_transcript_count;
    char command_transcript[512][256];
} Model;
extern Model model;
void model_init(void);
void model_advance(u64 us);
void model_flush_console(RadarConsole *c);
void model_complete_descriptors(RadarDma *d,u32 packets);
void model_force_uart(const char *response);
void model_set_page(u8 page,u32 value);
void model_destroy_dma(RadarDma *d);
#endif
