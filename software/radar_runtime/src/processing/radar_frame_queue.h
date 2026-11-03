/* SPDX-License-Identifier: MIT */
#ifndef RADAR_FRAME_QUEUE_H
#define RADAR_FRAME_QUEUE_H
#include "radar_dsp_types.h"
#define RADAR_FRAME_QUEUE_CAPACITY 16U
typedef struct {
    RadarFrameRef item[RADAR_FRAME_QUEUE_CAPACITY];
    uint32_t head,count,lease,generation,published,released,high_water;
    uint64_t last_trusted_us;
    uint8_t have_trusted_time;
} RadarFrameQueue;
void radar_frame_queue_init(RadarFrameQueue *q);
int radar_frame_queue_publish(RadarFrameQueue *q,const RadarFrameRef *frame);
const RadarFrameRef *radar_frame_queue_acquire(RadarFrameQueue *q);
int radar_frame_queue_release(RadarFrameQueue *q,uint32_t frame_id);
/* Explicit cancel drops references only when no consumer lease remains. */
int radar_frame_queue_clear(RadarFrameQueue *q);
#endif
