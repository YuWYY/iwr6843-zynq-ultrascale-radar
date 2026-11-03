/* SPDX-License-Identifier: MIT */
#ifndef RADAR_TIME_BRIDGE_H
#define RADAR_TIME_BRIDGE_H
#include "radar_dsp.h"

/* ABI 0x00010001. These registers observe SYS accepted FIFO->DMA beats;
 * they do not timestamp RF transmission, ADC sampling or antenna arrival. */
#define RADAR_TIME_ABI UINT32_C(0x00010001)
#define RADAR_TIME_CMD 0x128U
#define RADAR_TIME_GENERATION 0x12cU
#define RADAR_TIME_TICKS_LO 0x130U
#define RADAR_TIME_TICKS_HI 0x134U
#define RADAR_TIME_SNAPSHOT_EPOCH 0x138U
#define RADAR_TIME_STATUS 0x13cU
#define RADAR_TIME_EPOCH_TOKEN 0x140U
#define RADAR_TIME_CURRENT_EPOCH 0x144U
#define RADAR_TIME_REFRESH_US UINT64_C(500000)
#define RADAR_TIME_MAX_AGE_US UINT64_C(1000000)
#define RADAR_TIME_MAX_UNCERTAINTY_US 1000U
/* Explicit conservative model, checked against successive real anchors.
 * This is an engineering timing budget, not an oscillator calibration. */
#define RADAR_TIME_DRIFT_PPM 100U

enum RadarTimeRc {
    RADAR_TIME_OK=0, RADAR_TIME_E_ARGUMENT=-451,
    RADAR_TIME_E_CAPABILITY=-452, RADAR_TIME_E_EPOCH=-453,
    RADAR_TIME_E_SNAPSHOT=-454, RADAR_TIME_E_UNCERTAINTY=-455,
    RADAR_TIME_E_STALE=-456, RADAR_TIME_E_CLOCK=-457,
    RADAR_TIME_E_OWNER=-458
};
typedef struct {
    RadarDspIo io;
    uint64_t anchor_tick,anchor_us,bracket_before_us,bracket_after_us;
    uint32_t epoch,generation,hz,anchor_uncertainty_us;
    uint32_t refreshes,invalidations;
    int valid,error;
} RadarTimeBridge;
void radar_time_bridge_init(RadarTimeBridge *,const RadarDspIo *);
/* Called once before producer ARM. An empty armed DMA ring may already RUN;
 * no accepted beat or old live acquisition may exist at this point. */
int radar_time_bridge_begin(RadarTimeBridge *,uint32_t epoch);
/* source_and_dma_stopped must reflect actual confirmed hardware state.
 * Releases ownership, retains metadata and mapping for tail-frame evidence. */
int radar_time_bridge_end(RadarTimeBridge *,uint32_t epoch,int source_and_dma_stopped);
int radar_time_bridge_refresh(RadarTimeBridge *);
int radar_time_bridge_map(RadarTimeBridge *,uint32_t epoch,uint64_t sys_tick,
                          uint64_t now_us,uint64_t *ps_us,uint32_t *uncertainty_us);
void radar_time_bridge_invalidate(RadarTimeBridge *,int reason);

/* Single capture owner. END is legal only after verified PL stop, sensorStop
 * acknowledgement, complete-packet drain and capture DMA halt. */
int radar_stream_time_begin(uint32_t epoch);
int radar_stream_time_end(uint32_t epoch);
int radar_stream_time_pop(RadarTransportTime *,uint64_t *ps_us,uint32_t *uncertainty_us);
const RadarTimeBridge *radar_stream_time_state(void);
#endif
