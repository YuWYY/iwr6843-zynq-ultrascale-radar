/* SPDX-License-Identifier: MIT */
#ifndef RADAR_TIMING_H
#define RADAR_TIMING_H
#include <stdint.h>
#define RADAR_TIMING_CAPACITY 128U

/* PS times below are observations of the same monotonic A53 clock. Transport
 * ticks have their own origin/rate and must never be subtracted from PS time. */
enum {
    RADAR_TIMING_BEGIN=1U, RADAR_TIMING_FRONT_DONE=2U,
    RADAR_TIMING_SUBMITTED=4U, RADAR_TIMING_DONE=8U,
    RADAR_TIMING_POST_DONE=16U, RADAR_TIMING_PUBLISHED=32U,
    RADAR_TIMING_ACKED=64U, RADAR_TIMING_FAILED=128U,
    RADAR_TIMING_TRANSPORT=256U
};
typedef struct {
    uint32_t sequence,frame_id,data_revision,view_revision,page,coalesced;
    uint64_t render_begin_ps_us,render_end_ps_us,queue_ps_us,ack_ps_us;
} RadarTimingAck;
typedef struct {
    uint64_t sequence;
    uint32_t frame_id,acquisition_id,frame_index,backend,crc32,frame_flags;
    uint32_t stages,job_id,config_epoch,published_revision;
    uint32_t queue_depth,queue_high_water,presentation_count;
    int32_t error;
    uint32_t transport_sequence,transport_bytes,transport_hz,transport_status;
    uint64_t transport_first_tick,transport_last_tick;
    uint64_t observed_ps_us,batch_begin_ps_us,armed_ps_us,halt_ps_us;
    uint64_t copy_begin_ps_us,copy_complete_ps_us,ready_ps_us;
    uint64_t processing_begin_ps_us,front_begin_ps_us,front_end_ps_us;
    uint64_t submit_begin_ps_us,submit_end_ps_us,done_ps_us;
    uint64_t post_done_ps_us,published_ps_us,pl_cycles;
    RadarTimingAck first_ack;
} RadarTimingRecord;
typedef struct {
    RadarTimingRecord record[RADAR_TIMING_CAPACITY];
    uint32_t head,count,current_slot,last_ack_sequence;
    uint64_t written,overwritten,published,failed;
    uint64_t ack_events,first_presented,repeat_presented,unmatched_acks;
    uint64_t invalid_acks,nonframe_acks;
} RadarTimingLog;
void radar_timing_init(RadarTimingLog *);
void radar_timing_clear(RadarTimingLog *);
RadarTimingRecord *radar_timing_begin(RadarTimingLog *,uint32_t frame_id);
RadarTimingRecord *radar_timing_current(RadarTimingLog *);
const RadarTimingRecord *radar_timing_get(const RadarTimingLog *,uint32_t oldest_index);
/* One descriptor ACK may be observed repeatedly by the main loop. It counts
 * once; later page-only ACKs do not rewrite the frame's first presentation. */
void radar_timing_ack(RadarTimingLog *,const RadarTimingAck *);
#endif
