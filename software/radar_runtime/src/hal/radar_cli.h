/* SPDX-License-Identifier: MIT */
#ifndef RADAR_CLI_H
#define RADAR_CLI_H
#include "xuartps.h"
#include "radar_profile.h"
#include "radar_capture_profile.h"
#include <stddef.h>

typedef enum {
    RADAR_CLI_IDLE=0, RADAR_CLI_DRAIN, RADAR_CLI_TX,
    RADAR_CLI_RESPONSE, RADAR_CLI_SUCCESS, RADAR_CLI_FAILED
} RadarCliState;
typedef enum {
    RADAR_CLI_OK=0, RADAR_CLI_BUSY=1,
    RADAR_CLI_E_NOT_READY=-1, RADAR_CLI_E_PROFILE=-2,
    RADAR_CLI_E_BSP=-3, RADAR_CLI_E_UART=-4,
    RADAR_CLI_E_TX_TIMEOUT=-5, RADAR_CLI_E_TIMEOUT=-6,
    RADAR_CLI_E_OVERFLOW=-7, RADAR_CLI_E_REMOTE=-8,
    RADAR_CLI_E_PROTOCOL=-9, RADAR_CLI_E_CANCELED=-10,
    RADAR_CLI_E_NO_QUIET=-11, RADAR_CLI_E_RESET_REQUIRED=-12
} RadarCliResult;

typedef struct {
    XUartPs uart;
    RadarCliState state;
    int result;
    u8 initialized, enabled, needs_reset, terminal;
    u8 payload_seen, done_seen, prompt_seen, require_version, version_valid;
    u8 allow_finite_stop_warning;
    u8 require_actual_start;
    u32 finite_stop_warnings; /* Exact SDK warning lines in the current transaction. */
    u32 reset_epoch, version_epoch;
    char last_version[128];
    u32 tx_len, tx_sent, rx_len, line_len;
    u32 uart_errors, total_uart_errors, discarded_bytes;
    u32 queries, passed, failed;
    u64 started_us, stage_us, quiet_since_us, last_rx_us;
    char tx[RADAR_CLI_TX_CAPACITY];
    char rx[RADAR_CLI_RX_CAPACITY];
    char line[RADAR_CLI_LINE_CAPACITY];
} RadarCli;

void radar_cli_init(RadarCli *c); /* RAM only */
/* Call ONLY with PL UART output disabled. Does not clear needs_reset. */
int radar_cli_prepare(RadarCli *c);
void radar_cli_disable(RadarCli *c); /* PS UART only; caller must also gate PL */
int radar_cli_tx_idle(const RadarCli *c); /* FIFO empty AND shifter inactive */
int radar_cli_start_query(RadarCli *c, u64 now_us);
/* Sends ONLY a validated, compiled capture-profile command. The console is
 * never forwarded verbatim. Capture session is the sole caller/owner. */
int radar_cli_start_capture(RadarCli *c,const RadarCaptureProfile *profile,
                            RadarCaptureCommand kind,u32 index,u64 now_us);
void radar_cli_poll(RadarCli *c, u64 now_us); /* bounded RX/TX per invocation */
void radar_cli_cancel(RadarCli *c);
/* ONLY after explicit, verified physical NRST assert/release + boot wait.
 * TI CLI has no transaction ID: reconnect/quiet alone cannot discard an
 * arbitrarily late old reply after a timeout or a partially transmitted line. */
void radar_cli_new_reset_epoch(RadarCli *c);
int radar_cli_busy(const RadarCli *c);
int radar_cli_take_result(RadarCli *c, int *result);
const char *radar_cli_result_name(int result);
#endif
