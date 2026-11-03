/* SPDX-License-Identifier: MIT */
#ifndef RADAR_RANGE_QUALITY_H
#define RADAR_RANGE_QUALITY_H

#include "radar_range.h"
#include "radar_spatial_types.h"

typedef enum {
    RADAR_RANGE_QUALITY_NO_DATA = 0,
    RADAR_RANGE_QUALITY_RANGE_EDGE,
    RADAR_RANGE_QUALITY_CLIPPED,
    RADAR_RANGE_QUALITY_UNCONFIRMED,
    RADAR_RANGE_QUALITY_QUALIFIED,
    RADAR_RANGE_QUALITY_DETECTOR_UNAVAILABLE
} RadarRangeQuality;

/* Assess only the cached range frame's existing global peak. Never selects
 * another bin, changes a frame, or accesses hardware. QUALIFIED means a
 * distinct, interior, unclipped peak with a same-frame retained CFAR
 * candidate; it is not calibrated distance or an identified/tracked target.
 * UNCONFIRMED also covers a candidate absent from the retained top32, so it
 * must not be presented as proof that no physical target exists.
 * A frame timestamp of zero is valid for the first capture.
 */
RadarRangeQuality radar_range_assess(const RadarRangeFrame *range,
                                    const RadarSpatialFrame *spatial);

/* Stable ASCII status names for logs and offline evidence. */
const char *radar_range_quality_name(RadarRangeQuality quality);

#endif
