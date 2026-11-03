/* SPDX-License-Identifier: MIT */
#ifndef RADAR_DSP_H
#define RADAR_DSP_H
#include "radar_dsp_types.h"
typedef struct {
    void *context;
    uint32_t (*read)(void *,uint32_t);
    void (*write)(void *,uint32_t,uint32_t);
    void (*flush)(void *,uint32_t,uint32_t);
    void (*invalidate)(void *,uint32_t,uint32_t);
    void (*barrier)(void *);
    uint64_t (*now)(void *);
    /* Supplied from generated BSP address/size, never a user CLI override. */
    int mapped;
} RadarDspIo;
enum RadarDspRc {
    RADAR_DSP_PENDING=0,RADAR_DSP_DONE=1,
    RADAR_DSP_E_ARGUMENT=-401,RADAR_DSP_E_UNAVAILABLE=-402,
    RADAR_DSP_E_OWNER=-403,RADAR_DSP_E_IDENTITY=-404,
    RADAR_DSP_E_MEMORY=-405,RADAR_DSP_E_HARDWARE=-406,
    RADAR_DSP_E_TIMEOUT=-407,RADAR_DSP_E_CANCELED=-408,
    RADAR_DSP_E_LOCKED=-409,RADAR_DSP_E_RESULT=-410
};
typedef enum { RADAR_DSP_IDLE=0,RADAR_DSP_RUNNING,RADAR_DSP_CANCELING,
               RADAR_DSP_COMPLETE,RADAR_DSP_FAILED,RADAR_DSP_LOCKED,RADAR_DSP_RECOVERING } RadarDspState;
typedef struct {
    RadarDspIo io;
    RadarDspJob job;
    RadarDspResult result;
    RadarDspState state;
    uint64_t started_us,cancel_us;
    uint32_t submitted,completed,canceled,polls;
    uint32_t last_acquisition,last_frame_index,slot_lease[2];
    int error,capable,recoverable,need_temporal_reset;
} RadarDspDriver;
void radar_dsp_init(RadarDspDriver *,const RadarDspIo *);
/* Does not touch CSR when BSP did not declare this peripheral. */
int radar_dsp_probe(RadarDspDriver *);
void radar_dsp_job_layout(RadarDspJob *);
int radar_dsp_job_layout_slot(RadarDspJob *,uint32_t slot,uint32_t lease);
int radar_dsp_submit(RadarDspDriver *,const RadarDspJob *);
/* Laboratory packed-input submission. Requires DIAGNOSTIC_PACKED|CRC_VALID,
 * no HSI/time claim, PL_BASELINE and a dedicated, mutually exclusive driver.
 * Caller verifies actual packed bytes/CRC before entering this function. */
int radar_dsp_submit_diagnostic(RadarDspDriver *,const RadarDspJob *);
int radar_dsp_poll(RadarDspDriver *);
int radar_dsp_cancel(RadarDspDriver *);
/* Explicit local recovery only after hardware confirmed both AXI channels
 * halted (error bit31, BUSY=0). Never resets an outstanding transfer. */
int radar_dsp_recover(RadarDspDriver *);
const RadarDspResult *radar_dsp_result(const RadarDspDriver *);
int radar_dsp_busy(const RadarDspDriver *);
/* Native adapter. Does not probe/start hardware during init. */
void radar_dsp_platform_init(RadarDspDriver *);
typedef struct {
    uint32_t sequence,bytes,hz,status;
    uint64_t first_tick,last_tick;
    uint8_t valid;
} RadarTransportTime;
int radar_dsp_metadata_clear(RadarDspDriver *);
int radar_dsp_metadata_pop(RadarDspDriver *,RadarTransportTime *);
int radar_dsp_transport_begin(void);
int radar_dsp_transport_pop(RadarTransportTime *);
#endif
