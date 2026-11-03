/* SPDX-License-Identifier: MIT */
#include "radar_range_quality.h"
#include <math.h>

RadarRangeQuality radar_range_assess(const RadarRangeFrame *range,
                                    const RadarSpatialFrame *spatial)
{
    uint32_t bin, i;
    float center, left, right;
    if (!range || !range->valid) return RADAR_RANGE_QUALITY_NO_DATA;
    bin = range->peak_bin;
    if (!range->session || bin < RADAR_RANGE_FIRST_BIN || bin > RADAR_RANGE_LAST_BIN ||
        range->hsi_records != RADAR_RANGE_CHIRPS ||
        range->fft_count != RADAR_RANGE_CHIRPS * RADAR_RANGE_RX)
        return RADAR_RANGE_QUALITY_NO_DATA;
    for (i = 0U; i < RADAR_RANGE_TX; ++i)
        if (range->tx_records[i] != RADAR_CAPTURE_GEOMETRY_LOOPS)
            return RADAR_RANGE_QUALITY_NO_DATA;

    /* Both adjacent bins exist even at the configured search boundaries;
     * the raw spectrum itself is larger than the eligible search interval. */
    center = range->power_dbfs[bin];
    left = range->power_dbfs[bin - 1U];
    right = range->power_dbfs[bin + 1U];
    if (!isfinite(range->bin_m) || range->bin_m <= 0.0f ||
        fabsf(range->bin_m - (float)RADAR_RANGE_BIN_METERS) > 1.0e-6f ||
        !isfinite(range->peak_dbfs) || !isfinite(center) ||
        !isfinite(left) || !isfinite(right))
        return RADAR_RANGE_QUALITY_UNCONFIRMED;
    /* A zero-input frame defaults its argmax to the first search bin.
     * That default is no evidence of a near-range response. */
    if (range->peak_dbfs != center || center <= RADAR_RANGE_FLOOR_DBFS)
        return RADAR_RANGE_QUALITY_UNCONFIRMED;
    if (range->clipped_components) return RADAR_RANGE_QUALITY_CLIPPED;
    /* An actual local maximum on either search edge also stays unresolved.
     * Do not quietly expand or reduce the currently configured range. */
    if (bin == RADAR_RANGE_FIRST_BIN || bin == RADAR_RANGE_LAST_BIN)
        return RADAR_RANGE_QUALITY_RANGE_EDGE;
    if (center <= left || center <= right)
        return RADAR_RANGE_QUALITY_UNCONFIRMED;

    if (!spatial || !spatial->valid || spatial->error ||
        spatial->session != range->session ||
        spatial->timestamp_us != range->timestamp_us ||
        spatial->count > RADAR_SPATIAL_MAX_POINTS ||
        spatial->candidate_count < spatial->count ||
        spatial->truncated_count != spatial->candidate_count - spatial->count)
        return RADAR_RANGE_QUALITY_DETECTOR_UNAVAILABLE;
    for (i = 0U; i < spatial->count; ++i) {
        const RadarSpatialPoint *point = &spatial->points[i];
        /* AoA quality, XYZ validity, calibration and Doppler do not gate
         * the range peak. In particular, zero-Doppler returns remain valid. */
        if (point->range_bin == bin && isfinite(point->power_dbfs) &&
            isfinite(point->snr_db))
            return RADAR_RANGE_QUALITY_QUALIFIED;
    }
    return RADAR_RANGE_QUALITY_UNCONFIRMED;
}

const char *radar_range_quality_name(RadarRangeQuality quality)
{
    switch (quality) {
    case RADAR_RANGE_QUALITY_NO_DATA: return "NO_DATA";
    case RADAR_RANGE_QUALITY_RANGE_EDGE: return "RANGE_EDGE";
    case RADAR_RANGE_QUALITY_CLIPPED: return "CLIPPED";
    case RADAR_RANGE_QUALITY_UNCONFIRMED: return "UNCONFIRMED";
    case RADAR_RANGE_QUALITY_QUALIFIED: return "QUALIFIED";
    case RADAR_RANGE_QUALITY_DETECTOR_UNAVAILABLE: return "DETECTOR_UNAVAILABLE";
    default: return "UNKNOWN";
    }
}
