/* SPDX-License-Identifier: MIT */
#include "radar_freshness.h"
#include <string.h>

void radar_freshness_capture(RadarSampleFreshness *out,const RadarFrameRef *f,
    RadarDspBackend backend,uint32_t result_flags,uint32_t origin,int valid)
{
    memset(out,0,sizeof *out);if(!f)return;
    out->frame_id=f->frame_id;out->acquisition_id=f->acquisition_id;out->frame_index=f->frame_index;
    out->time_origin=origin;out->backend=backend;out->result_flags=result_flags;out->frame_flags=f->flags;
    out->timestamp_us=f->timestamp_us;out->observed_us=f->observed_us;out->observed_valid=1U;
    out->sample_valid=!!valid && (f->flags&3U)==3U &&
        !(f->flags&(RADAR_FRAME_CLIPPED|RADAR_FRAME_DISCONTINUITY)) &&
        !(result_flags&(RADAR_DSP_RESULT_INVALID_INPUT|RADAR_DSP_RESULT_IDENTITY_RESET|
            RADAR_DSP_RESULT_CELL_ORDER_ERROR|UINT32_C(0x80000100))); /* fatal/saturated numerical result */
    out->source=(f->flags&RADAR_FRAME_TIME_TRANSPORT)?RADAR_TIME_TRANSPORT_SYS_FIRST_BEAT:
        ((f->flags&RADAR_FRAME_TIME_TRUSTED)?RADAR_TIME_PL_BATCH_FIRST:RADAR_TIME_DMA_COMPLETION_OBSERVED);
    out->deadline_valid=out->sample_valid && origin && (f->flags&RADAR_FRAME_TIME_TRUSTED) &&
        !(f->flags&(RADAR_FRAME_CLIPPED|RADAR_FRAME_DISCONTINUITY)) &&
        !(result_flags&RADAR_DSP_RESULT_TIME_UNTRUSTED);
    if(out->deadline_valid)out->valid_until_us=f->timestamp_us>UINT64_MAX-RADAR_SAMPLE_CURRENT_US?
        UINT64_MAX:f->timestamp_us+RADAR_SAMPLE_CURRENT_US;
}
RadarFreshness radar_freshness_evaluate(const RadarSampleFreshness *f,const RadarFreshnessNow *now)
{
    RadarFreshness r={0};
    if(!f || !f->frame_id)return r;
    r.sample_valid=f->sample_valid;r.state=f->sample_valid?RADAR_FRESH_UNAVAILABLE:RADAR_FRESH_INVALID_SAMPLE;
    if(!now)return r;
    if(f->observed_valid && now->observed_now_valid && now->observed_now_us>=f->observed_us) {
        r.observed_age_valid=1U;r.observed_age_lower_bound_us=now->observed_now_us-f->observed_us;
    }
    if(!f->sample_valid)return r;
    /* A late host observation cannot make a sample fresh. Conversely, once
     * even this lower age bound exceeds the cap, expiration is certain. */
    if(r.observed_age_valid && r.observed_age_lower_bound_us>RADAR_SAMPLE_CURRENT_US) {
        r.state=RADAR_FRESH_EXPIRED;return r;
    }
    if(f->deadline_valid && now->sample_now_trusted && now->time_origin==f->time_origin &&
       now->sample_source==f->source && now->sample_now_us>=f->timestamp_us) {
        if(now->sample_now_us>f->valid_until_us)r.state=RADAR_FRESH_EXPIRED;
        else {r.state=RADAR_FRESH_CURRENT;r.current_eligible=1U;}
    }
    return r;
}
const char *radar_freshness_name(RadarFreshnessState s)
{
    static const char *const names[]={"NO_SAMPLE","INVALID_SAMPLE","UNAVAILABLE","CURRENT","EXPIRED"};
    return (unsigned)s<sizeof names/sizeof names[0]?names[s]:"UNAVAILABLE";
}
const char *radar_sample_time_name(RadarSampleTimeSource s)
{
    static const char *const names[]={"UNKNOWN","PL_BATCH_FIRST","TRANSPORT_SYS_FIRST_BEAT","DMA_COMPLETION_OBSERVED"};
    return (unsigned)s<sizeof names/sizeof names[0]?names[s]:"UNKNOWN";
}
