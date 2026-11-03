/* SPDX-License-Identifier: MIT
 * RAM-only telemetry and production scheduling tests. No GPIO/button/DP IO. */
#include "radar_timing.h"
#include "radar_view_schedule.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static RadarTimingLog logbook;
static RadarTimingAck ack(uint32_t sequence,uint32_t frame,uint32_t revision)
{
    RadarTimingAck a={0};
    a.sequence=sequence;a.frame_id=frame;a.data_revision=revision;
    a.view_revision=1U;a.render_begin_ps_us=300U;a.render_end_ps_us=400U;
    a.queue_ps_us=500U;a.ack_ps_us=600U;return a;
}
static RadarTimingRecord *published(uint32_t frame,uint32_t revision)
{
    RadarTimingRecord *r=radar_timing_begin(&logbook,frame);
    assert(r);r->stages|=RADAR_TIMING_PUBLISHED;r->published_revision=revision;
    r->published_ps_us=200U;return r;
}
static void ring_and_clocks(void)
{
    unsigned n;
    radar_timing_init(&logbook);
    assert(!radar_timing_current(&logbook)&&!radar_timing_get(&logbook,0U));
    assert(!radar_timing_begin(&logbook,0U));
    for(n=1U;n<=300U;++n) {
        RadarTimingRecord *r=published(n,n);
        r->acquisition_id=(n-1U)/16U+1U;r->frame_index=(n-1U)%16U;
        r->transport_hz=100000000U;r->transport_first_tick=UINT64_C(9000000000)+n;
        r->transport_last_tick=r->transport_first_tick+100U;r->observed_ps_us=n*100U;
    }
    assert(logbook.count==128U&&logbook.written==300U&&logbook.overwritten==172U);
    for(n=0U;n<128U;++n) {
        const RadarTimingRecord *r=radar_timing_get(&logbook,n);
        assert(r->sequence==173U+n&&r->frame_id==173U+n);
        assert(r->frame_index==(r->frame_id-1U)%16U);
        assert(r->transport_first_tick==UINT64_C(9000000000)+r->frame_id);
        assert(r->observed_ps_us==r->frame_id*100U);
    }
    assert(!radar_timing_get(&logbook,128U));
    { RadarTimingAck a=ack(1U,1U,1U);radar_timing_ack(&logbook,&a); }
    assert(logbook.unmatched_acks==1U); /* Overwritten data is not relabeled. */
}
static void ack_identity_and_first_presentation(void)
{
    RadarTimingRecord *r;RadarTimingAck a;
    radar_timing_init(&logbook);r=published(15U,7U);
    a=ack(1U,15U,8U); /* Same frame, later COMPLETE status revision. */
    radar_timing_ack(&logbook,&a);radar_timing_ack(&logbook,&a);
    assert(logbook.first_presented==1U&&logbook.ack_events==1U&&r->presentation_count==1U);
    a=ack(2U,15U,8U);a.page=2U;a.view_revision=9U;a.ack_ps_us=999U;
    radar_timing_ack(&logbook,&a);
    assert(logbook.repeat_presented==1U&&r->first_ack.ack_ps_us==600U&&r->presentation_count==2U);
    a=ack(3U,15U,6U);radar_timing_ack(&logbook,&a);
    a=ack(4U,15U,7U);a.queue_ps_us=399U;radar_timing_ack(&logbook,&a);
    a=ack(5U,15U,7U);a.render_begin_ps_us=199U;radar_timing_ack(&logbook,&a);
    assert(logbook.invalid_acks==3U);
    a=ack(6U,0U,0U);radar_timing_ack(&logbook,&a);assert(logbook.nonframe_acks==1U);
    a=ack(7U,16U,7U);radar_timing_ack(&logbook,&a);assert(logbook.unmatched_acks==1U);
    radar_timing_clear(&logbook);radar_timing_ack(&logbook,&a);
    assert(!logbook.count&&!logbook.ack_events&&logbook.last_ack_sequence==7U);
    r=published(17U,9U);r->published_ps_us=0U;
    a=ack(8U,17U,9U);a.render_begin_ps_us=a.render_end_ps_us=a.queue_ps_us=a.ack_ps_us=0U;
    radar_timing_ack(&logbook,&a);assert(r->stages&RADAR_TIMING_ACKED);
}
static void independent_page_and_data(void)
{
    RadarViewSchedule view;RadarTimingAck a;RadarTimingRecord *r;
    radar_timing_init(&logbook);radar_view_schedule_init(&view);
    r=published(20U,10U);
    radar_view_schedule_data(&view,10U);
    radar_view_schedule_page(&view,RADAR_VIEW_AZIMUTH);
    radar_view_schedule_page(&view,RADAR_VIEW_TOP); /* Coalesced final request. */
    assert(radar_view_schedule_submit(&view));
    a=ack(1U,20U,view.submitted.data_revision);
    a.page=(uint32_t)view.submitted.page;a.view_revision=view.submitted.view_revision;
    radar_view_schedule_page(&view,RADAR_VIEW_SPACE);
    assert(radar_view_schedule_ack(&view));radar_timing_ack(&logbook,&a);
    assert(r->first_ack.page==RADAR_VIEW_TOP&&r->first_ack.data_revision==10U);
    assert(!radar_view_schedule_needs_render(&view,1)); /* LIVE adds no extra render. */
    assert(radar_view_schedule_needs_render(&view,0));
    assert(radar_view_schedule_submit(&view));
    a=ack(2U,20U,view.submitted.data_revision);
    a.page=(uint32_t)view.submitted.page;a.view_revision=view.submitted.view_revision;
    assert(radar_view_schedule_ack(&view));radar_timing_ack(&logbook,&a);
    assert(logbook.first_presented==1U&&logbook.repeat_presented==1U);
    assert(r->first_ack.page==RADAR_VIEW_TOP); /* Page redraw never changes frame latency. */
    assert(view.presented.page==RADAR_VIEW_SPACE);
}
int main(void)
{
    ring_and_clocks();ack_identity_and_first_presentation();independent_page_and_data();
    printf("PASS telemetry: bounded ring, frame identity, raw clock domains, first ACK, status/page revision, LIVE coalescing; record=%zu ring=%zu bytes\n",
        sizeof(RadarTimingRecord),sizeof(RadarTimingLog));
    return 0;
}
