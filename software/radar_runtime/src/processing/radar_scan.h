/* SPDX-License-Identifier: MIT */
#ifndef RADAR_SCAN_H
#define RADAR_SCAN_H
#include "radar_session.h"
#include "radar_range.h"
#include "radar_spatial.h"
#include "radar_dsp.h"
#include "radar_frame_queue.h"
#include "radar_freshness.h"
#include "radar_timing.h"

#define RADAR_SCAN_HISTORY 128U
#define RADAR_SCAN_DEFAULT_FRAMES 16U
#define RADAR_SCAN_MAX_FRAMES 32U
#define RADAR_SCAN_PROFILE_ID RADAR_CAPTURE_GEOMETRY_PROFILE_ID
#define RADAR_SCAN_SPATIAL_SLICE_US UINT64_C(500)
#define RADAR_SCAN_SPATIAL_MAX_STEPS 256U

typedef enum {
    RADAR_SCAN_IDLE=0, RADAR_SCAN_WAIT_INITIAL_PRESENT,
    RADAR_SCAN_CAPTURE, RADAR_SCAN_PROCESS, RADAR_SCAN_WAIT_FRAME_PRESENT,
    RADAR_SCAN_BEGIN_QUALIFY, RADAR_SCAN_QUALIFY,
    RADAR_SCAN_CONNECT, RADAR_SCAN_QUERY
} RadarScanState;

typedef struct {
    /* Oldest to newest. Samples are measured finite captures, not uniform
     * cfg-period samples. The renderer must use timestamp_us for its axis. */
    RadarRangeFrame history[RADAR_SCAN_HISTORY];
    u32 count,revision,presented_revision;
    const char *status; /* Short ASCII status suitable for the DP renderer. */
    u32 batch_total,batch_done,last_session,sys_hz; /* total=0 for live */
    u8 active,stop_requested,live_mode;

    RadarSession *session;
    RadarRangeProcessor processor;
    /* Static RadarScan ownership: these bounded workspaces never live on the
     * stack. The published frame is immutable until the next data revision. */
    RadarSpatialProcessor spatial_processor;
    RadarSpatialFrame spatial_frame;
    u64 capture_observed_us,published_observed_us;
    u32 published_session;
    RadarSampleFreshness published_freshness;
    int spatial_error;
    /* Per-frame cooperative-work telemetry. Time starts before the existing
     * frame/ownership guard. A started atomic step may finish after500us;
     * no subsequent step starts then. Publication/logging is outside this
     * work slice. The256-call bound also applies if the timer stalls. */
    u32 spatial_calls,spatial_slices;
    u64 spatial_max_slice_us;
    RadarScanState state;
    u64 phase_us,origin_tick,last_tick,processing_tick;
    u32 processing_session,processing_crc,processing_record,expected_crc;
    u8 batch_mode,have_origin,resume_empty;
    RadarDspDriver dsp;
    RadarDspBackend backend;
    RadarFrameQueue frames;
    RadarFrameRef processing_frame;
    uint32_t next_frame_id,last_acquisition,processing_acquisition,dsp_phase,pl_import_row;
    uint32_t dsp_job_id,dsp_config_epoch,source_crc,view_skipped;
    uint64_t origin_observed_us,previous_dsp_time;
    uint64_t transport_origin_tick;
    uint64_t transport_origin_us;
    uint32_t transport_hz;
    uint8_t display_decoupled,cancel_pending,have_previous_dsp_time;
    RadarCaptureResult consumer_result;
    RadarScanState producer_state;
    uint64_t producer_phase_us;
    uint32_t prefetched_batches;
    uint8_t pipeline;
    /* Bounded diagnostic history only; no new data/display scheduling gate. */
    RadarTimingLog timing;
} RadarScan;

void radar_scan_init(RadarScan *scan,RadarSession *session);
/* Call after the existing single-owner session / SW3 / SW2 poll dispatch.
 * This scheduler never polls those services or touches DMA payload buffers. */
void radar_scan_poll(RadarScan *scan);
/* 1: handled or blocked; 0: delegate to the existing command dispatcher.
 * While active, only read-only inquiries and immediate safe/disable pass. */
int radar_scan_command(RadarScan *scan,const char *command);
/* Call only with the revision whose DP descriptor was actually acknowledged,
 * not merely rendered or submitted. Legacy mode gates capture; explicitly
 * decoupled mode records presentation without granting raw-buffer ownership. */
void radar_scan_presented(RadarScan *scan,u32 revision);
/* Feed the RAM-only descriptor observation after radar_display_poll. This
 * records evidence and never grants a capture or DSP buffer lease. */
void radar_scan_display_ack(RadarScan *scan,const RadarTimingAck *ack);
int radar_scan_busy(const RadarScan *scan);
/* Temporal tau2s coefficient, nonnegative nearest-even UQ1.31. */
uint32_t radar_scan_alpha_q31(uint64_t delta_us);
#endif
