/* SPDX-License-Identifier: MIT */
#ifndef RADAR_SCOPE_VIEW_H
#define RADAR_SCOPE_VIEW_H
#include <stdint.h>
#include "radar_range.h"
#include "radar_spatial_types.h"
#include "radar_scene_types.h"
#include "radar_view_types.h"
#define RADAR_SCOPE_WIDTH 1920U
#define RADAR_SCOPE_HEIGHT 1080U
#define RADAR_SCOPE_HISTORY_MAX 128U
/* Pure painter. Caller owns an inactive 1920x1080 RGBA8888 framebuffer,
 * flushes it, and submits it only after painting returns. The history is
 * chronological, immutable during this call, and contains measured frames.
 * Empty data draws an explicit waiting state; no demonstration samples exist.
 * Time columns use measured timestamps, never the cfg nominal frame period.
 * Spatial data must match the latest history session and PL-relative time.
 * Nominal uncalibrated points are labelled; invalid XYZ never draws at z=0. */
void radar_scope_render(uint32_t *pixels, unsigned stride_words,
                        const RadarRangeFrame *history, unsigned count,
                        const char *status, RadarViewPage page,
                        const RadarViewDiagnostics *diagnostics,
                        const RadarSpatialFrame *spatial);
/* Scene is an immutable matching snapshot. NULL draws an explicit unavailable
 * scene state; the painter never runs the tracker or any signal processing. */
void radar_scope_render_scene(uint32_t *pixels, unsigned stride_words,
                        const RadarRangeFrame *history, unsigned count,
                        const char *status, RadarViewPage page,
                        const RadarViewDiagnostics *diagnostics,
                        const RadarSpatialFrame *spatial,const RadarSceneFrame *scene);
#endif
