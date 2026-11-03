/* SPDX-License-Identifier: MIT
 * Pure scene processing. The caller owns the workspace in reserved DDR and
 * keeps input immutable until DONE/cancel. No heap, IO, DMA, keys or hardware.
 */
#ifndef RADAR_SCENE_H
#define RADAR_SCENE_H
#include <stddef.h>
#include "radar_spatial_types.h"
#include "radar_scene_types.h"
enum RadarSceneRc { RADAR_SCENE_BUSY=0, RADAR_SCENE_DONE=1,
    RADAR_SCENE_E_ARGUMENT=-1, RADAR_SCENE_E_STATE=-2,
    RADAR_SCENE_E_IDENTITY=-3, RADAR_SCENE_E_TIME=-4 };
typedef struct { void *workspace; size_t workspace_bytes; } RadarSceneProcessor;
size_t radar_scene_workspace_bytes(void);
const char *radar_scene_parameter_identity(void);
/* Recorder initialization seed; resets preserve it, initialization starts at1. */
uint32_t radar_scene_reflection_next_id(const RadarSceneProcessor *);
int radar_scene_init(RadarSceneProcessor *p,void *workspace,size_t bytes);
void radar_scene_reset(RadarSceneProcessor *p,uint32_t reason);
int radar_scene_begin(RadarSceneProcessor *p,const RadarSpatialFrame *spatial,
                      const RadarSceneContext *context);
/* One heatmap row, one bounded clustering/association pass or one 256-point
 * line-fit trial per call. Consumers may yield between every call. */
int radar_scene_step(RadarSceneProcessor *p);
void radar_scene_cancel(RadarSceneProcessor *p);
const RadarSceneFrame *radar_scene_result(const RadarSceneProcessor *p);
const char *radar_scene_label_name(uint32_t label);
#endif
