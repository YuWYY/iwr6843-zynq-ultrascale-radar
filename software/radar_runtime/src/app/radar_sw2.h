/* SPDX-License-Identifier: MIT */
#ifndef RADAR_SW2_H
#define RADAR_SW2_H
#include "radar_control.h"
#include "radar_cli.h"
#include "radar_console.h"

typedef enum {
    SW2_SAFE=0, SW2_CONNECT_WAIT, SW2_ONLINE, SW2_QUERY,
    SW2_REPEAT_WAIT, SW2_RESET_HOLD, SW2_RESET_BOOT, SW2_FAULT, SW2_CAPTURE
} RadarSw2State;
typedef enum { SW2_JOB_NONE=0, SW2_JOB_QUERY, SW2_JOB_CHECK10,
               SW2_JOB_RESET, SW2_JOB_RESET3 } RadarSw2Job;

typedef struct {
    RadarControl *control;
    RadarConsole *console;
    RadarCli cli;
    RadarSw2State state;
    RadarSw2Job job;
    u32 expected_io,flags,io_read,job_done,job_total;
    u32 last_gpio_rc, last_error;
    /* Completed batches in THIS boot; not fabricated board verification. */
    u32 check10_passes, reset3_passes, verified_safe_returns;
    u64 stage_us,last_monitor_us,last_alive_us;
    u8 wiring_confirmed,io_verified,cli_verified,fatal_control;
} RadarSw2;

/* Called only after SW0 and the read-only SW1 checks succeed on a cold start.
 * This phase never claims DMA quiescence; do not attach to live acquisition. */
void radar_sw2_init(RadarSw2 *s,RadarControl *control,RadarConsole *console);
void radar_sw2_command(RadarSw2 *s,const char *line);
void radar_sw2_poll(RadarSw2 *s);
/* Exclusive transfer to SW4. While claimed, legacy SW2 poll/commands are
 * inert; SW4 services UART and checks per-stage power/IO/capture invariants. */
/* Preflight checks ownership/power/IO without enabling outputs or claiming. */
int radar_sw2_capture_check(RadarSw2 *s);
int radar_sw2_capture_claim(RadarSw2 *s);
int radar_sw2_capture_io(RadarSw2 *s,u16 request);
int radar_sw2_capture_release(RadarSw2 *s,int fault);
const char *radar_sw2_state_name(RadarSw2State state);
#endif
