/* SPDX-License-Identifier: MIT */
#include "radar_view_schedule.h"
#include <string.h>

void radar_view_schedule_init(RadarViewSchedule *v)
{
    memset(v,0,sizeof(*v));
    v->requested.page=RADAR_VIEW_OVERVIEW;
    v->requested.view_revision=1U;
}

void radar_view_schedule_data(RadarViewSchedule *v,uint32_t revision)
{
    if(revision) v->requested.data_revision=revision;
}

void radar_view_schedule_page(RadarViewSchedule *v,RadarViewPage page)
{
    if((unsigned)page>=RADAR_VIEW_COUNT || page==v->requested.page) return;
    v->requested.page=page;
    if(++v->requested.view_revision==0U) ++v->requested.view_revision;
}

int radar_view_schedule_needs_render(const RadarViewSchedule *v,int scan_active)
{
    return !v->pending && v->requested.data_revision &&
        (v->requested.data_revision!=v->presented.data_revision ||
         (!scan_active && v->requested.view_revision!=v->presented.view_revision));
}

int radar_view_schedule_submit(RadarViewSchedule *v)
{
    if(v->pending || !v->requested.data_revision) return 0;
    v->submitted=v->requested;
    v->pending=1U;
    return 1;
}

int radar_view_schedule_ack(RadarViewSchedule *v)
{
    if(!v->pending) return 0;
    v->presented=v->submitted;
    v->pending=0U;
    return 1;
}

void radar_view_schedule_forget_presentation(RadarViewSchedule *v)
{
    memset(&v->submitted,0,sizeof(v->submitted));
    memset(&v->presented,0,sizeof(v->presented));
    v->pending=0U;
}

const char *radar_view_page_name(RadarViewPage page)
{
    static const char *const names[]={"overview","azimuth","top","space"};
    return (unsigned)page<RADAR_VIEW_COUNT ? names[page] : "invalid";
}
