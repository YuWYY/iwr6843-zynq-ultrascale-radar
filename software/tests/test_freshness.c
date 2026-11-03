/* SPDX-License-Identifier: MIT: pure time qualification, no key/board tests. */
#include "radar_freshness.h"
#include "radar_view_cache.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static RadarViewSnapshot first,second;
static RadarRangeFrame history[1];
static RadarSpatialFrame spatial;
static RadarSampleFreshness make_sample(uint32_t flags,uint32_t result_flags)
{
    RadarFrameRef f={0};RadarSampleFreshness sample;
    f.frame_id=9U;f.acquisition_id=4U;f.frame_index=2U;
    f.flags=flags;f.timestamp_us=1000000U;f.observed_us=9000000U;
    radar_freshness_capture(&sample,&f,PL_ENHANCED,result_flags,7U,1);
    return sample;
}
int main(void)
{
    RadarSampleFreshness sample=make_sample(39U,0U),copy;
    RadarFreshnessNow now={0};RadarFreshness r;
    RadarViewCache cache;RadarViewDiagnostics d={0},aged;const RadarViewSnapshot *snapshot;
    unsigned i;
    assert(sample.sample_valid&&sample.deadline_valid&&sample.valid_until_us==1500000U);
    assert(sample.source==RADAR_TIME_TRANSPORT_SYS_FIRST_BEAT);
    copy=sample;
    r=radar_freshness_evaluate(&sample,&now);assert(r.state==RADAR_FRESH_UNAVAILABLE&&!r.current_eligible);
    now.sample_now_trusted=1U;now.sample_source=sample.source;now.time_origin=sample.time_origin;
    now.sample_now_us=999999U;r=radar_freshness_evaluate(&sample,&now);assert(r.state==RADAR_FRESH_UNAVAILABLE);
    now.sample_now_us=1000000U;r=radar_freshness_evaluate(&sample,&now);assert(r.current_eligible);
    now.sample_now_us=1500000U;r=radar_freshness_evaluate(&sample,&now);assert(r.state==RADAR_FRESH_CURRENT);
    now.sample_now_us=1500001U;r=radar_freshness_evaluate(&sample,&now);assert(r.state==RADAR_FRESH_EXPIRED&&!r.current_eligible);
    now.sample_now_us=9000000U;r=radar_freshness_evaluate(&sample,&now);assert(r.state==RADAR_FRESH_EXPIRED);
    assert(!memcmp(&copy,&sample,sizeof sample)); /* No new frame cannot renew a deadline. */
    now.sample_now_us=1100000U;now.time_origin=8U;r=radar_freshness_evaluate(&sample,&now);assert(r.state==RADAR_FRESH_UNAVAILABLE);
    now.time_origin=7U;now.sample_source=RADAR_TIME_PL_BATCH_FIRST;
    r=radar_freshness_evaluate(&sample,&now);assert(r.state==RADAR_FRESH_UNAVAILABLE);
    /* Current application has no sample-clock correlation. Short observed age
     * is only a lower bound; it cannot establish CURRENT. */
    memset(&now,0,sizeof now);now.observed_now_valid=1U;now.observed_now_us=9000000U;
    r=radar_freshness_evaluate(&sample,&now);assert(r.observed_age_valid&&r.state==RADAR_FRESH_UNAVAILABLE);
    now.observed_now_us=9500000U;r=radar_freshness_evaluate(&sample,&now);assert(r.state==RADAR_FRESH_UNAVAILABLE);
    now.observed_now_us=9500001U;r=radar_freshness_evaluate(&sample,&now);assert(r.state==RADAR_FRESH_EXPIRED);
    now.observed_now_us=8999999U;r=radar_freshness_evaluate(&sample,&now);assert(!r.observed_age_valid&&r.state==RADAR_FRESH_UNAVAILABLE);
    sample=make_sample(3U,0x4c0U);now.observed_now_us=9000010U;
    now.sample_now_trusted=1U;now.sample_now_us=1100000U;now.time_origin=7U;now.sample_source=sample.source;
    r=radar_freshness_evaluate(&sample,&now);assert(sample.sample_valid&&!sample.deadline_valid&&r.state==RADAR_FRESH_UNAVAILABLE);
    now.observed_now_us=9500001U;r=radar_freshness_evaluate(&sample,&now);assert(r.state==RADAR_FRESH_EXPIRED);
    /* Integrity/clip/discontinuity/numeric failures never become CURRENT just
     * because a matching clock was supplied. Warmup/fallback remains separate. */
    memset(&now,0,sizeof now);now.sample_now_trusted=1U;now.sample_now_us=1100000U;
    now.sample_source=RADAR_TIME_TRANSPORT_SYS_FIRST_BEAT;now.time_origin=7U;
    for(i=0U;i<8U;++i) {
        const uint32_t frame_flags[]={38U,37U,47U,55U,39U,39U,39U,39U};
        const uint32_t result_flags[]={0U,0U,0U,0U,4U,8U,256U,512U};
        sample=make_sample(frame_flags[i],result_flags[i]);r=radar_freshness_evaluate(&sample,&now);
        assert(!sample.sample_valid&&!r.current_eligible&&r.state==RADAR_FRESH_INVALID_SAMPLE);
    }
    sample=make_sample(39U,0xc3U);r=radar_freshness_evaluate(&sample,&now);
    assert(r.current_eligible&&sample.result_flags==0xc3U&&!sample.detection_count);
    /* A sample may be recent with zero detections and baseline fallback. Time
     * qualification says nothing about target confidence/enhanced readiness. */
    radar_view_cache_init(&cache,&first,&second);d.freshness=sample;
    d.sampled_us=9000100U;d.frame_age_valid=1U;d.frame_age_us=100U;
    history[0].session=spatial.session=sample.frame_id;
    assert(!radar_view_cache_publish(&cache,history,1U,&spatial,&d,"LIVE",1U));
    snapshot=radar_view_cache_acquire(&cache);copy=snapshot->diagnostics.freshness;
    d.freshness.frame_id=10U;d.freshness.valid_until_us=99000000U;d.freshness.backend=PS_REFERENCE;
    assert(!radar_view_cache_publish(&cache,history,1U,&spatial,&d,"LIVE",2U));
    radar_view_cache_diagnostics(snapshot,9500001U,&aged);
    assert(!memcmp(&copy,&aged.freshness,sizeof copy)&&!memcmp(&copy,&snapshot->diagnostics.freshness,sizeof copy));
    memset(&now,0,sizeof now);now.observed_now_valid=1U;now.observed_now_us=9500001U;
    r=radar_freshness_evaluate(&aged.freshness,&now);assert(r.state==RADAR_FRESH_EXPIRED);
    radar_view_cache_diagnostics(snapshot,12000000U,&aged);
    assert(aged.freshness.valid_until_us==1500000U&&aged.freshness.frame_id==9U);
    assert(!radar_view_cache_release(&cache,1U));
    memset(&sample,0,sizeof sample);assert(radar_freshness_evaluate(&sample,&now).state==RADAR_FRESH_NO_SAMPLE);
    puts("PASS freshness same-domain deadline, no-frame expiry, sample-only observer, integrity gates and immutable snapshot");
    return 0;
}
