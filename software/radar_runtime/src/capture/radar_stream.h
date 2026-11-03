/* SPDX-License-Identifier: MIT */
#ifndef RADAR_STREAM_H
#define RADAR_STREAM_H
#include "radar_session.h"
#include "radar_raw_slots.h"
typedef struct {
    void *context;
    int (*validate_frame)(void *,const void *,u32,u64 *);
    int (*transport_begin)(void *,u32 capture_epoch);
    int (*transport_pop)(void *,RadarTransportTime *,u64 *first_us,u32 *uncertainty_us);
    int (*transport_end)(void *,u32 capture_epoch);
} RadarStreamIo;
enum RadarStreamRc { RADAR_STREAM_E_METADATA=-501,RADAR_STREAM_E_OVERLOAD=-502,
    RADAR_STREAM_E_STALE=-503,RADAR_STREAM_E_HSI=-504,RADAR_STREAM_E_CAPABILITY=-505 };
typedef struct {
    RadarSession *session;
    RadarStreamIo io;
    RadarRawSlots raw;
    RadarDmaPacket copy_packet;
    RadarStreamFrame *writing;
    RadarTransportTime metadata[32];
    u32 metadata_sequence[32];
    u64 metadata_first_us[32];
    u32 metadata_error_us[32];
    u32 metadata_received,copy_fragment,copy_offset,copy_bytes,copy_crc;
    u32 frame_limit,frames_verified,frames_released,frames_residual,stop_target;
    u32 max_backlog;
    u32 stop_unused_bds; /* preposted empty BDs discarded by confirmed soft reset */
    int stop_reason;
    u32 copy_cpu_us,health_max_us;
    u64 health_cpu_us;
    u64 last_progress_us,stop_us,copy_done_us;
    int error,native_error;
    u8 active,copy_lease,stop_requested,stop_sent,metadata_open,boundary_seen,stop_by_reset;
} RadarStream;
void radar_stream_init(RadarStream *,RadarSession *,const RadarStreamIo *);
/* Caller selects one strict stream profile, confirms it and supplies native
 * capabilities before start. frame_limit is 16,64 or0 (continuous). */
int radar_stream_start(RadarStream *,u32 frame_limit);
void radar_stream_poll(RadarStream *);
/* Normal stop is packet boundary -> sensorStop ACK -> verified drain proof.
 * Preposted unused BDs use one native graceful software reset; an exhausted
 * finite tail uses HALT then reset. Neither releases memory/metadata before
 * ResetIsDone AND hardware Halted and ring reconstruction are confirmed.
 * Nonzero reason preserves an unsuccessful terminal result; never restarts. */
int radar_stream_request_stop(RadarStream *,int reason);
void radar_stream_cancel(RadarStream *);
int radar_stream_acquire(RadarStream *,RadarStreamFrame *);
int radar_stream_release(RadarStream *,const RadarStreamFrame *);
int radar_stream_active(const RadarStream *);
int radar_stream_display_idle(const RadarStream *);
/* Only after explicit session recovery restored isolated IDLE/READY. */
int radar_stream_clear_fault(RadarStream *);
#endif
