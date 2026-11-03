/* SPDX-License-Identifier: MIT */
#ifndef RADAR_VIEW_SCHEDULE_H
#define RADAR_VIEW_SCHEDULE_H
#include "radar_view_types.h"

typedef struct {
    uint32_t data_revision, view_revision;
    RadarViewPage page;
} RadarViewTag;

/* Pure bookkeeping. Only the display owner submits and acknowledges tags. */
typedef struct {
    RadarViewTag requested, submitted, presented;
    unsigned pending;
} RadarViewSchedule;

void radar_view_schedule_init(RadarViewSchedule *v);
void radar_view_schedule_data(RadarViewSchedule *v, uint32_t revision);
void radar_view_schedule_page(RadarViewSchedule *v, RadarViewPage page);
int radar_view_schedule_needs_render(const RadarViewSchedule *v, int scan_active);
int radar_view_schedule_submit(RadarViewSchedule *v);
int radar_view_schedule_ack(RadarViewSchedule *v);
void radar_view_schedule_forget_presentation(RadarViewSchedule *v);
const char *radar_view_page_name(RadarViewPage page);
#endif
