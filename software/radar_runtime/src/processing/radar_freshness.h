/* SPDX-License-Identifier: MIT */
#ifndef RADAR_FRESHNESS_H
#define RADAR_FRESHNESS_H
#include "radar_dsp_types.h"

#define RADAR_SAMPLE_CURRENT_US UINT64_C(500000)
typedef enum {
    RADAR_TIME_UNKNOWN=0, RADAR_TIME_PL_BATCH_FIRST,
    RADAR_TIME_TRANSPORT_SYS_FIRST_BEAT, RADAR_TIME_DMA_COMPLETION_OBSERVED
} RadarSampleTimeSource;
typedef enum {
    RADAR_FRESH_NO_SAMPLE=0, RADAR_FRESH_INVALID_SAMPLE,
    RADAR_FRESH_UNAVAILABLE, RADAR_FRESH_CURRENT, RADAR_FRESH_EXPIRED
} RadarFreshnessState;

/* Immutable publication metadata. valid_until is a software current-sample
 * eligibility cap, not a claim to export every cell's PL protect_until. */
typedef struct {
    uint32_t frame_id,acquisition_id,frame_index,time_origin;
    RadarDspBackend backend;
    RadarSampleTimeSource source;
    uint32_t result_flags,frame_flags,sample_valid,deadline_valid,observed_valid;
    uint32_t spatial_valid,detection_count;
    uint64_t timestamp_us,valid_until_us,observed_us;
} RadarSampleFreshness;
typedef struct {
    uint64_t observed_now_us,sample_now_us;
    uint32_t observed_now_valid,sample_now_trusted,time_origin;
    RadarSampleTimeSource sample_source;
} RadarFreshnessNow;
typedef struct {
    RadarFreshnessState state;
    uint32_t sample_valid,current_eligible,observed_age_valid;
    uint64_t observed_age_lower_bound_us;
} RadarFreshness;

void radar_freshness_capture(RadarSampleFreshness *,const RadarFrameRef *,
    RadarDspBackend,uint32_t result_flags,uint32_t time_origin,int sample_valid);
/* Pure evaluation: never changes/renews a stored timestamp or deadline.
 * CURRENT is frame time eligibility, never target confidence, detection count,
 * enhanced-mode readiness, or permission to renew PL background protection.
 * It needs an explicitly trusted now with identical source and origin.
 * DMA-observation age only bounds sample age from below, never above. */
RadarFreshness radar_freshness_evaluate(const RadarSampleFreshness *,const RadarFreshnessNow *);
const char *radar_freshness_name(RadarFreshnessState);
const char *radar_sample_time_name(RadarSampleTimeSource);
#endif
