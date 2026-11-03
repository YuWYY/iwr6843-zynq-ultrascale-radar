/* SPDX-License-Identifier: MIT */
#ifndef RADAR_VIEW_CACHE_H
#define RADAR_VIEW_CACHE_H
#include "radar_view_types.h"
#include "radar_range.h"
#include "radar_spatial_types.h"
#include "radar_scene_types.h"
#define RADAR_VIEW_CACHE_HISTORY 128U
typedef struct {
    RadarRangeFrame history[RADAR_VIEW_CACHE_HISTORY];
    RadarSpatialFrame spatial;
    RadarSceneFrame scene;
    RadarViewDiagnostics diagnostics;
    char status[40];
    uint32_t count,revision;
} RadarViewSnapshot;
typedef struct {
    RadarViewSnapshot *slot[2];
    int latest,leased;
    uint32_t published,rendered,coalesced,last_rendered_revision;
} RadarViewCache;
void radar_view_cache_init(RadarViewCache *,RadarViewSnapshot *,RadarViewSnapshot *);
int radar_view_cache_publish(RadarViewCache *,const RadarRangeFrame *,uint32_t,
    const RadarSpatialFrame *,const RadarViewDiagnostics *,const char *,uint32_t);
const RadarViewSnapshot *radar_view_cache_acquire(RadarViewCache *);
int radar_view_cache_release(RadarViewCache *,uint32_t);
void radar_view_cache_diagnostics(const RadarViewSnapshot *,uint64_t,RadarViewDiagnostics *);
#endif
