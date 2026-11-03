/* SPDX-License-Identifier: MIT */
#include "radar_frame_queue.h"
#include "radar_view_cache.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static RadarViewSnapshot a,b;
static RadarRangeFrame history[128];
static RadarSpatialFrame spatial;
int main(void)
{
    RadarFrameQueue q;RadarFrameRef f={0};RadarViewCache c;RadarViewDiagnostics d={0};
    const RadarFrameRef *held;const RadarViewSnapshot *view;unsigned i;
    radar_frame_queue_init(&q);f.raw=(const uint8_t *)"x";f.bytes=399360;f.flags=RADAR_FRAME_CRC_VALID;f.acquisition_id=1U;
    for(i=0;i<16U;++i){f.frame_id=i+1U;f.frame_index=i;f.timestamp_us=i;assert(radar_frame_queue_publish(&q,&f)==0);}
    assert(radar_frame_queue_publish(&q,&f)<0&&q.count==16U);
    held=radar_frame_queue_acquire(&q);assert(held&&held->frame_id==1U);
    assert(!radar_frame_queue_acquire(&q)&&radar_frame_queue_clear(&q)<0);
    assert(radar_frame_queue_release(&q,2U)<0&&held->frame_id==1U);
    assert(!radar_frame_queue_release(&q,1U));
    for(i=2U;i<=16U;++i){held=radar_frame_queue_acquire(&q);assert(held->frame_id==i);assert(!radar_frame_queue_release(&q,i));}
    assert(!q.count&&!q.lease&&q.high_water==16U);
    /* Four more acquisitions reuse the ring addresses without reusing IDs. */
    for(i=17U;i<=80U;++i) {
        f.frame_id=i;f.acquisition_id=2U+(i-17U)/16U;f.frame_index=(i-17U)%16U;f.timestamp_us=i*100000U;
        assert(!radar_frame_queue_publish(&q,&f));held=radar_frame_queue_acquire(&q);
        assert(held->frame_id==i&&held->acquisition_id==f.acquisition_id);
        assert(!radar_frame_queue_release(&q,i));
    }
    assert(q.published==80U&&q.released==80U&&!q.count&&!q.lease);
    radar_view_cache_init(&c,&a,&b);history[0].session=1;spatial.session=1;
    assert(!radar_view_cache_publish(&c,history,1,&spatial,&d,"LIVE",1));view=radar_view_cache_acquire(&c);
    history[0].session=2;spatial.session=2;
    assert(!radar_view_cache_publish(&c,history,1,&spatial,&d,"LIVE",2));
    assert(view->history[0].session==1&&view->spatial.session==1);
    history[0].session=3;spatial.session=3;
    assert(!radar_view_cache_publish(&c,history,1,&spatial,&d,"LIVE",3));assert(view->revision==1);
    assert(c.coalesced==1U);
    assert(!radar_view_cache_release(&c,1));view=radar_view_cache_acquire(&c);
    assert(view->revision==3&&view->history[0].session==3);assert(!radar_view_cache_release(&c,3));
    assert(radar_view_cache_acquire(&c)->revision==3);assert(!radar_view_cache_release(&c,3));
    radar_view_cache_init(&c,&a,&b);
    assert(!radar_view_cache_publish(&c,history,1,&spatial,&d,"LIVE",UINT32_MAX));
    assert(!radar_view_cache_publish(&c,history,1,&spatial,&d,"LIVE",1U));
    assert(radar_view_cache_publish(&c,history,1,&spatial,&d,"LIVE",UINT32_MAX)<0);
    b.diagnostics.frame_age_valid=1U;b.diagnostics.frame_age_us=1000U;b.diagnostics.sampled_us=100U;
    b.diagnostics.last_session=37U;radar_view_cache_diagnostics(&b,5100U,&d);
    assert(d.frame_age_valid&&d.frame_age_us==6000U&&d.last_session==37U);
    assert(b.diagnostics.frame_age_us==1000U);
    radar_view_cache_diagnostics(&b,99U,&d);assert(!d.frame_age_valid);
    b.diagnostics.frame_age_us=UINT64_MAX;radar_view_cache_diagnostics(&b,101U,&d);assert(!d.frame_age_valid);
    puts("PASS immutable frame leases and latest-view coalescing while renderer owns prior snapshot");return 0;
}
