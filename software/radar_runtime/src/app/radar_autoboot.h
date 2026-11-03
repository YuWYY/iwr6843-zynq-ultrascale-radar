/* SPDX-License-Identifier: MIT */
#ifndef RADAR_AUTOBOOT_H
#define RADAR_AUTOBOOT_H
#include "radar_stream_scan.h"
#ifndef RADAR_AUTOBOOT_ENABLE
#define RADAR_AUTOBOOT_ENABLE 1
#endif
/* This deployment approval is tied to the user's confirmed F20 installation,
 * not a voltage measurement. Changed wiring/firmware requires manual review. */
#define RADAR_AUTOBOOT_PROFILE "ba6ddefbcaf3defb"
typedef enum {
    RADAR_BOOT_MANUAL=0, RADAR_BOOT_SETTLE, RADAR_BOOT_BACKEND,
    RADAR_BOOT_DECOUPLE, RADAR_BOOT_MODE, RADAR_BOOT_PROFILE,
    RADAR_BOOT_IO, RADAR_BOOT_DMA, RADAR_BOOT_CONNECT,
    RADAR_BOOT_CHECK10, RADAR_BOOT_RESET3, RADAR_BOOT_CONFIRM,
    RADAR_BOOT_PREFLIGHT, RADAR_BOOT_HPD, RADAR_BOOT_DISPLAY,
    RADAR_BOOT_DP_ACK, RADAR_BOOT_VIEW, RADAR_BOOT_LIVE,
    RADAR_BOOT_FIRST_FRAME, RADAR_BOOT_DONE, RADAR_BOOT_FAILED,
    RADAR_BOOT_CANCELED
} RadarAutobootStage;
typedef struct {
    RadarScan *scan;
    RadarStreamScan *stream;
    RadarConsole *console;
    RadarAutobootStage stage,failed_stage;
    u64 began_us,stage_us,last_us,timeout_us,stable_us;
    u32 prior_count,dp_observations,dp_startup_flags;
    int error;
    u8 issued,have_stable,dp_baseline_valid;
} RadarAutoboot;
void radar_autoboot_init(RadarAutoboot *,RadarScan *,RadarStreamScan *,RadarConsole *,int enabled);
void radar_autoboot_poll(RadarAutoboot *);
/* Read-only inquiries leave boot running. An explicit control/config command
 * cancels boot once, cleans up via existing ownership paths, then delegates. */
int radar_autoboot_command(RadarAutoboot *,const char *);
int radar_autoboot_active(const RadarAutoboot *);
const char *radar_autoboot_stage_name(RadarAutobootStage);
#endif
