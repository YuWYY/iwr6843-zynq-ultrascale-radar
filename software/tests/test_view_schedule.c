/* SPDX-License-Identifier: MIT
 * Tests the view/data scheduling API, not any button, GPIO or debounce code. */
#include "radar_view_schedule.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void initial_and_pending(void)
{
    RadarViewSchedule v;
    RadarViewTag sent;
    radar_view_schedule_init(&v);
    assert(v.requested.page==RADAR_VIEW_OVERVIEW);
    assert(!radar_view_schedule_needs_render(&v,0));
    assert(!radar_view_schedule_submit(&v));
    assert(!radar_view_schedule_ack(&v));
    radar_view_schedule_data(&v,1U);
    assert(radar_view_schedule_needs_render(&v,1));
    assert(radar_view_schedule_submit(&v));
    sent=v.submitted;
    radar_view_schedule_page(&v,RADAR_VIEW_TOP);
    assert(!radar_view_schedule_needs_render(&v,0));
    assert(!radar_view_schedule_submit(&v));
    assert(!memcmp(&sent,&v.submitted,sizeof(sent)));
    assert(radar_view_schedule_ack(&v));
    assert(v.presented.data_revision==1U);
    assert(v.presented.page==RADAR_VIEW_OVERVIEW);
    assert(!radar_view_schedule_needs_render(&v,1));
    assert(radar_view_schedule_needs_render(&v,0));
}

static void page_change_never_adds_live_render(void)
{
    RadarViewSchedule v;
    unsigned frame,i,renders=0U;
    radar_view_schedule_init(&v);
    for(frame=1U;frame<=140U;++frame){
        /* Request changes throughout a capture's conceptual busy phases.
         * They must not make any additional data revision or rendering. */
        for(i=0U;i<9U;++i){
            radar_view_schedule_page(&v,(RadarViewPage)((frame+i)%RADAR_VIEW_COUNT));
            assert(v.requested.data_revision==frame-1U);
            assert(!radar_view_schedule_needs_render(&v,1));
        }
        radar_view_schedule_data(&v,frame);
        assert(radar_view_schedule_needs_render(&v,1));
        assert(radar_view_schedule_submit(&v));
        ++renders;
        {
            RadarViewTag sent=v.submitted;
            radar_view_schedule_page(&v,(RadarViewPage)((sent.page+1U)%RADAR_VIEW_COUNT));
            assert(!radar_view_schedule_needs_render(&v,1));
            assert(radar_view_schedule_ack(&v));
            assert(v.presented.data_revision==frame);
            assert(v.presented.view_revision==sent.view_revision);
            assert(v.presented.page==sent.page);
        }
        assert(!radar_view_schedule_needs_render(&v,1));
    }
    assert(renders==140U);
    assert(v.presented.data_revision==140U);
}

static void idle_render_and_new_data_during_pending(void)
{
    RadarViewSchedule v;
    uint32_t data;
    radar_view_schedule_init(&v);
    radar_view_schedule_data(&v,8U);
    assert(radar_view_schedule_submit(&v));
    assert(radar_view_schedule_ack(&v));
    data=v.presented.data_revision;
    radar_view_schedule_page(&v,RADAR_VIEW_SPACE);
    assert(radar_view_schedule_needs_render(&v,0));
    assert(radar_view_schedule_submit(&v));
    radar_view_schedule_page(&v,RADAR_VIEW_AZIMUTH);
    radar_view_schedule_data(&v,9U);
    assert(radar_view_schedule_ack(&v));
    assert(v.presented.data_revision==data);
    assert(v.presented.page==RADAR_VIEW_SPACE);
    assert(radar_view_schedule_needs_render(&v,1));
    assert(radar_view_schedule_submit(&v));
    assert(radar_view_schedule_ack(&v));
    assert(v.presented.data_revision==9U);
    assert(v.presented.page==RADAR_VIEW_AZIMUTH);
    assert(!radar_view_schedule_needs_render(&v,0));
}

static void duplicate_invalid_wrap_and_restart(void)
{
    RadarViewSchedule v;
    radar_view_schedule_init(&v);
    radar_view_schedule_data(&v,99U);
    radar_view_schedule_data(&v,0U);
    assert(v.requested.data_revision==99U);
    radar_view_schedule_page(&v,RADAR_VIEW_OVERVIEW);
    radar_view_schedule_page(&v,(RadarViewPage)-1);
    radar_view_schedule_page(&v,RADAR_VIEW_COUNT);
    assert(v.requested.view_revision==1U);
    assert(v.requested.page==RADAR_VIEW_OVERVIEW);
    v.requested.view_revision=UINT32_MAX;
    radar_view_schedule_page(&v,RADAR_VIEW_TOP);
    assert(v.requested.view_revision==1U);
    assert(radar_view_schedule_submit(&v));
    radar_view_schedule_forget_presentation(&v);
    assert(!radar_view_schedule_ack(&v));
    assert(v.presented.data_revision==0U && !v.pending);
    assert(v.requested.page==RADAR_VIEW_TOP);
    assert(radar_view_schedule_needs_render(&v,1));
    assert(radar_view_schedule_submit(&v));
    assert(radar_view_schedule_ack(&v));
    assert(!strcmp(radar_view_page_name(v.presented.page),"top"));
}

int main(void)
{
    initial_and_pending();
    page_change_never_adds_live_render();
    idle_render_and_new_data_during_pending();
    duplicate_invalid_wrap_and_restart();
    puts("PASS: independent page/data tuple ACK; 140 live frames with 140 renders; idle pages; pending/duplicate/wrap/restart. No button tests.");
    return 0;
}
