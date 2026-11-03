/* SPDX-License-Identifier: MIT
 * Bounded point-only LOS reflection tracking. No hardware, allocation, heatmap
 * or geometric height assumptions. Suitable for independent recorded replay.
 */
#ifndef RADAR_REFLECTION_H
#define RADAR_REFLECTION_H
#include "radar_scene_types.h"
#include "radar_spatial_types.h"
#define RADAR_REFLECTION_SAMPLES 32U
typedef struct { uint64_t time_us; float range_m; } RadarReflectionSample;
typedef struct {
    RadarStableReflection value;
    RadarReflectionSample samples[RADAR_REFLECTION_SAMPLES];
    uint32_t alive,count;
} RadarReflectionTrack;
typedef struct {
    RadarReflectionTrack tracks[RADAR_SCENE_MAX_REFLECTIONS];
    uint32_t next_id;
    RadarSceneContext context;
    uint32_t have_context;
} RadarReflectionProcessor;
void radar_reflection_reset(RadarReflectionProcessor *p);
void radar_reflection_update(RadarReflectionProcessor *p,const RadarSpatialFrame *s,
                             const RadarSceneContext *c,RadarSceneFrame *out);
#endif
