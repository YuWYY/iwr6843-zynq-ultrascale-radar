/* SPDX-License-Identifier: MIT */
#include "radar_timing.h"
#include <stddef.h>
#include <string.h>

void radar_timing_init(RadarTimingLog *log)
{
    if(log)memset(log,0,sizeof *log);
}
void radar_timing_clear(RadarTimingLog *log)
{
    uint32_t last;
    if(!log)return;
    last=log->last_ack_sequence;
    radar_timing_init(log);log->last_ack_sequence=last;
}
RadarTimingRecord *radar_timing_begin(RadarTimingLog *log,uint32_t frame_id)
{
    RadarTimingRecord *r;
    uint32_t slot;
    if(!log || !frame_id)return NULL;
    if(log->count==RADAR_TIMING_CAPACITY) {
        slot=log->head;log->head=(log->head+1U)%RADAR_TIMING_CAPACITY;
        ++log->overwritten;
    } else {
        slot=(log->head+log->count)%RADAR_TIMING_CAPACITY;++log->count;
    }
    log->current_slot=slot;r=&log->record[slot];memset(r,0,sizeof *r);
    r->sequence=++log->written;r->frame_id=frame_id;r->stages=RADAR_TIMING_BEGIN;
    return r;
}
RadarTimingRecord *radar_timing_current(RadarTimingLog *log)
{
    return log && log->count ? &log->record[log->current_slot] : NULL;
}
const RadarTimingRecord *radar_timing_get(const RadarTimingLog *log,uint32_t index)
{
    return log && index<log->count ? &log->record[(log->head+index)%RADAR_TIMING_CAPACITY] : NULL;
}
void radar_timing_ack(RadarTimingLog *log,const RadarTimingAck *ack)
{
    uint32_t i;
    if(!log || !ack || !ack->sequence || ack->sequence==log->last_ack_sequence)return;
    log->last_ack_sequence=ack->sequence;++log->ack_events;
    if(!ack->frame_id) { ++log->nonframe_acks;return; }
    if(!ack->data_revision || ack->render_end_ps_us<ack->render_begin_ps_us ||
       ack->queue_ps_us<ack->render_end_ps_us || ack->ack_ps_us<ack->queue_ps_us) {
        ++log->invalid_acks;return;
    }
    for(i=0U;i<log->count;++i) {
        RadarTimingRecord *r=&log->record[(log->head+log->count-1U-i)%RADAR_TIMING_CAPACITY];
        if(r->frame_id!=ack->frame_id)continue;
        /* A final STOPPED/COMPLETE status may legitimately publish a later
         * revision of the same frame before its first rendering. */
        if(!(r->stages&RADAR_TIMING_PUBLISHED) || ack->data_revision<r->published_revision ||
           ack->render_begin_ps_us<r->published_ps_us) {
            ++log->invalid_acks;return;
        }
        ++r->presentation_count;
        if(r->stages&RADAR_TIMING_ACKED) { ++log->repeat_presented;return; }
        r->first_ack=*ack;r->stages|=RADAR_TIMING_ACKED;++log->first_presented;return;
    }
    ++log->unmatched_acks;
}
