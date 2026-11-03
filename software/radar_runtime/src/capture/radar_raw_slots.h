/* SPDX-License-Identifier: MIT */
#ifndef RADAR_RAW_SLOTS_H
#define RADAR_RAW_SLOTS_H
#include "radar_dsp.h"
#include "xil_types.h"
#define RADAR_RAW_SLOT_COUNT 16U
#define RADAR_RAW_SLOT_BYTES UINT32_C(0x80000)
#define RADAR_STREAM_FRAME_BYTES 399360U
typedef enum { RADAR_RAW_FREE=0,RADAR_RAW_WRITING,RADAR_RAW_VERIFIED,RADAR_RAW_PROCESSING } RadarRawState;
typedef struct {
    UINTPTR raw;
    u32 slot,lease,capture_epoch,transport_sequence,frame_id,bytes,crc32,dma_epoch;
    u64 observed_us,verified_us,hsi_id,first_accept_us;
    u32 time_error_us;
    u32 copy_cpu_us;
    RadarTransportTime transport;
} RadarStreamFrame;
typedef struct { RadarStreamFrame frame;RadarRawState state; } RadarRawSlot;
typedef struct {
    RadarRawSlot slots[RADAR_RAW_SLOT_COUNT];
    UINTPTR base;
    u32 next_lease,next_frame_id,ready,processing,used,high_water;
} RadarRawSlots;
void radar_raw_slots_init(RadarRawSlots *,UINTPTR base);
/* Reset refuses outstanding consumers. Generations never restart. */
int radar_raw_slots_reset(RadarRawSlots *);
int radar_raw_slots_reserve(RadarRawSlots *,u32 epoch,u32 sequence,u32 dma_epoch,u64 observed_us,RadarStreamFrame **);
int radar_raw_slots_publish(RadarRawSlots *,const RadarStreamFrame *);
int radar_raw_slots_discard_write(RadarRawSlots *,const RadarStreamFrame *);
int radar_raw_slots_acquire(RadarRawSlots *,RadarStreamFrame *);
int radar_raw_slots_release(RadarRawSlots *,const RadarStreamFrame *);
u64 radar_raw_slots_oldest_us(const RadarRawSlots *);
#endif
