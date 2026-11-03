/* SPDX-License-Identifier: MIT */
#include "radar_frame_queue.h"
#include <string.h>
void radar_frame_queue_init(RadarFrameQueue *q) { memset(q,0,sizeof *q);q->generation=1U; }
int radar_frame_queue_publish(RadarFrameQueue *q,const RadarFrameRef *f)
{
    uint32_t tail;
    if(!q || !f || !f->raw || f->bytes!=399360U || !f->frame_id || !f->acquisition_id ||
       !(f->flags&RADAR_FRAME_CRC_VALID) || q->count==RADAR_FRAME_QUEUE_CAPACITY)return -1;
    if(q->count) {
        const RadarFrameRef *last=&q->item[(q->head+q->count-1U)%RADAR_FRAME_QUEUE_CAPACITY];
        if(f->frame_id<=last->frame_id)return -1;
    }
    if((f->flags&RADAR_FRAME_TIME_TRUSTED) && q->have_trusted_time &&
       f->timestamp_us<q->last_trusted_us)return -1;
    if(f->flags&RADAR_FRAME_TIME_TRUSTED) { q->last_trusted_us=f->timestamp_us;q->have_trusted_time=1U; }
    tail=(q->head+q->count)%RADAR_FRAME_QUEUE_CAPACITY;q->item[tail]=*f;++q->count;++q->published;
    if(q->count>q->high_water)q->high_water=q->count;
    return 0;
}
const RadarFrameRef *radar_frame_queue_acquire(RadarFrameQueue *q)
{
    if(!q || !q->count || q->lease)return NULL;
    q->lease=q->item[q->head].frame_id;return &q->item[q->head];
}
int radar_frame_queue_release(RadarFrameQueue *q,uint32_t id)
{
    if(!q || !q->lease || q->lease!=id || !q->count || q->item[q->head].frame_id!=id)return -1;
    memset(&q->item[q->head],0,sizeof q->item[q->head]);q->head=(q->head+1U)%RADAR_FRAME_QUEUE_CAPACITY;
    --q->count;q->lease=0U;++q->released;return 0;
}
int radar_frame_queue_clear(RadarFrameQueue *q)
{
    if(!q || q->lease)return -1;
    q->head=q->count=0U;q->have_trusted_time=0U;q->last_trusted_us=0U;
    if(++q->generation==0U)++q->generation;
    return 0;
}
