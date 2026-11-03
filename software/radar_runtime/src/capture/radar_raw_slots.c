/* SPDX-License-Identifier: MIT */
#include "radar_raw_slots.h"
#include <string.h>
static int matches(const RadarRawSlot *s,const RadarStreamFrame *f)
{
    return s->frame.slot==f->slot && s->frame.lease==f->lease &&
        s->frame.capture_epoch==f->capture_epoch && s->frame.frame_id==f->frame_id &&
        s->frame.transport_sequence==f->transport_sequence && s->frame.raw==f->raw;
}
void radar_raw_slots_init(RadarRawSlots *p,UINTPTR base)
{
    memset(p,0,sizeof(*p));p->base=base;p->next_lease=1U;p->next_frame_id=1U;
}
int radar_raw_slots_reset(RadarRawSlots *p)
{
    if(!p || p->used || p->ready || p->processing)return -1;
    memset(p->slots,0,sizeof(p->slots));return 0;
}
int radar_raw_slots_reserve(RadarRawSlots *p,u32 epoch,u32 seq,u32 dma_epoch,u64 time,RadarStreamFrame **out)
{
    u32 i;RadarRawSlot *s;
    if(!p || !out || !p->base || !epoch || !seq || !p->next_lease || !p->next_frame_id)return -1;
    for(i=0;i<RADAR_RAW_SLOT_COUNT;++i)if(p->slots[i].state==RADAR_RAW_FREE)break;
    if(i==RADAR_RAW_SLOT_COUNT)return 1;
    s=&p->slots[i];memset(&s->frame,0,sizeof(s->frame));s->state=RADAR_RAW_WRITING;
    s->frame.raw=p->base+(UINTPTR)i*RADAR_RAW_SLOT_BYTES;s->frame.slot=i;
    s->frame.lease=p->next_lease++;s->frame.frame_id=p->next_frame_id++;
    s->frame.capture_epoch=epoch;s->frame.transport_sequence=seq;s->frame.dma_epoch=dma_epoch;
    s->frame.observed_us=time;s->frame.bytes=RADAR_STREAM_FRAME_BYTES;
    ++p->used;if(p->used>p->high_water)p->high_water=p->used;*out=&s->frame;return 0;
}
int radar_raw_slots_publish(RadarRawSlots *p,const RadarStreamFrame *f)
{
    RadarRawSlot *s;
    if(!p || !f || f->slot>=RADAR_RAW_SLOT_COUNT)return -1;
    s=&p->slots[f->slot];if(s->state!=RADAR_RAW_WRITING || !matches(s,f))return -1;
    s->state=RADAR_RAW_VERIFIED;++p->ready;return 0;
}
int radar_raw_slots_discard_write(RadarRawSlots *p,const RadarStreamFrame *f)
{
    RadarRawSlot *s;
    if(!p || !f || f->slot>=RADAR_RAW_SLOT_COUNT)return -1;
    s=&p->slots[f->slot];if(s->state!=RADAR_RAW_WRITING || !matches(s,f))return -1;
    s->state=RADAR_RAW_FREE;--p->used;return 0;
}
int radar_raw_slots_acquire(RadarRawSlots *p,RadarStreamFrame *out)
{
    u32 i,best=RADAR_RAW_SLOT_COUNT;
    if(!p || !out)return -1;
    for(i=0;i<RADAR_RAW_SLOT_COUNT;++i)if(p->slots[i].state==RADAR_RAW_VERIFIED &&
        (best==RADAR_RAW_SLOT_COUNT || p->slots[i].frame.frame_id<p->slots[best].frame.frame_id))best=i;
    if(best==RADAR_RAW_SLOT_COUNT)return 1;
    p->slots[best].state=RADAR_RAW_PROCESSING;--p->ready;++p->processing;*out=p->slots[best].frame;return 0;
}
int radar_raw_slots_release(RadarRawSlots *p,const RadarStreamFrame *f)
{
    RadarRawSlot *s;
    if(!p || !f || f->slot>=RADAR_RAW_SLOT_COUNT)return -1;
    s=&p->slots[f->slot];if(s->state!=RADAR_RAW_PROCESSING || !matches(s,f))return -1;
    s->state=RADAR_RAW_FREE;--p->processing;--p->used;return 0;
}
u64 radar_raw_slots_oldest_us(const RadarRawSlots *p)
{
    u32 i;u64 t=UINT64_MAX;
    if(!p)return t;
    for(i=0;i<RADAR_RAW_SLOT_COUNT;++i)if((p->slots[i].state==RADAR_RAW_VERIFIED ||
        p->slots[i].state==RADAR_RAW_WRITING) && p->slots[i].frame.observed_us<t)t=p->slots[i].frame.observed_us;
    return t;
}
