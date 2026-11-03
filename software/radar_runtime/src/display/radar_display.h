/* SPDX-License-Identifier: MIT */
#ifndef RADAR_DISPLAY_H
#define RADAR_DISPLAY_H
#include "radar_console.h"
#include "radar_range.h"
#include "radar_view_types.h"
#include "radar_spatial_types.h"
#include "radar_timing.h"
#include "radar_scene_types.h"
/* Call once after cold-start memory provisioning. It does not enable DP. */
void radar_display_init(RadarConsole *console);
/* Suppress only normal per-frame ACK messages. Errors remain immediate. */
void radar_display_live_quiet(int enabled);
/* busy includes capture ownership and other radar control transactions.
 * While busy, existing pixels continue scanning; no AUX/train/draw occurs. */
void radar_display_poll(int capture_busy);
/* Returns 1 for every display command (including rejected requests). */
int radar_display_command(const char *command, int capture_busy);
/* Chronological measured frames remain owned by the cooperative scan task.
 * Rendering only occurs when idle, into the acknowledged non-front page. */
void radar_display_scope_update(const RadarRangeFrame *history, unsigned count,
                                const char *status, u32 revision,
                                int scan_active, const RadarViewDiagnostics *diagnostics,
                                const RadarSpatialFrame *spatial);
void radar_display_scope_scene_update(const RadarRangeFrame *,unsigned,const char *,u32,int,
    const RadarViewDiagnostics *,const RadarSpatialFrame *,const RadarSceneFrame *);
/* RAM-only requests. Active scans apply the latest page on the next normal
 * data render, never by delaying capture for an extra view-only rendering. */
void radar_display_request_page(RadarViewPage page);
RadarViewPage radar_display_get_requested_page(void);
/* Route before the scan command gate. Only handles display view commands and
 * never initializes hardware, changes RF, draws, or waits for a descriptor. */
int radar_display_view_command(const char *command);
u32 radar_display_scope_presented(void);
/* Cached observation of the last real descriptor ACK; no register access. */
const RadarTimingAck *radar_display_last_ack(void);
int radar_display_active(void);
/* Read-only pre-init HPD, no configure/AUX/training or retry. */
int radar_display_monitor_present(void);
/* True after a real descriptor ACK with no fatal/DPDMA error. Historical
 * TX FIFO bit8 is retained separately; callers must gate new observations. */
int radar_display_ready(void);
/* ISR nonzero observations, including startup; not distinct hardware events. */
u32 radar_display_fifo_observations(void);
u32 radar_display_faults(void); /* Cached flags, never performs AUX/MMIO. */
#endif
