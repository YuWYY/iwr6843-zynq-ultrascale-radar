/* SPDX-License-Identifier: MIT */
#include "radar_range_quality.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static RadarRangeFrame range_frame, range_before;
static RadarSpatialFrame spatial_frame, spatial_before;

static void fixture(uint32_t bin)
{
    unsigned i;
    memset(&range_frame, 0, sizeof(range_frame));
    memset(&spatial_frame, 0, sizeof(spatial_frame));
    range_frame.valid = spatial_frame.valid = 1U;
    range_frame.session = spatial_frame.session = 73U;
    range_frame.timestamp_us = spatial_frame.timestamp_us = UINT64_C(50123456);
    range_frame.bin_m = (float)RADAR_RANGE_BIN_METERS;
    range_frame.peak_bin = bin;
    range_frame.peak_dbfs = -40.0f;
    range_frame.hsi_records = RADAR_RANGE_CHIRPS;
    range_frame.fft_count = RADAR_RANGE_CHIRPS * RADAR_RANGE_RX;
    for (i = 0U; i < RADAR_RANGE_TX; ++i)
        range_frame.tx_records[i] = RADAR_CAPTURE_GEOMETRY_LOOPS;
    for (i = 0U; i < RADAR_RANGE_BINS; ++i) range_frame.power_dbfs[i] = -90.0f;
    range_frame.power_dbfs[bin] = range_frame.peak_dbfs;
    spatial_frame.count = spatial_frame.candidate_count = 1U;
    spatial_frame.points[0].range_bin = bin;
    spatial_frame.points[0].power_dbfs = -40.0f;
    spatial_frame.points[0].snr_db = 30.0f;
    spatial_frame.points[0].doppler_bin = 0;
    spatial_frame.points[0].azimuth_deg = NAN;
    spatial_frame.points[0].elevation_deg = NAN;
    spatial_frame.points[0].x_m = NAN;
    spatial_frame.points[0].y_m = NAN;
    spatial_frame.points[0].z_m = NAN;
    spatial_frame.points[0].flags = RADAR_POINT_LOW_COHERENCE | RADAR_POINT_ANGLE_EDGE;
}

static void expect(RadarRangeQuality wanted)
{
    memcpy(&range_before, &range_frame, sizeof(range_before));
    memcpy(&spatial_before, &spatial_frame, sizeof(spatial_before));
    assert(radar_range_assess(&range_frame, &spatial_frame) == wanted);
    assert(memcmp(&range_before, &range_frame, sizeof(range_before)) == 0);
    assert(memcmp(&spatial_before, &spatial_frame, sizeof(spatial_before)) == 0);
}

static void test_static_and_angle_independence(void)
{
    fixture(64U);
    expect(RADAR_RANGE_QUALITY_QUALIFIED);
    range_frame.timestamp_us = spatial_frame.timestamp_us = 0U;
    expect(RADAR_RANGE_QUALITY_QUALIFIED);
    spatial_frame.points[0].doppler_bin = -16;
    spatial_frame.points[0].flags |= RADAR_POINT_DOPPLER_EDGE;
    expect(RADAR_RANGE_QUALITY_QUALIFIED);
    spatial_frame.calibrated = 1U;
    expect(RADAR_RANGE_QUALITY_QUALIFIED);
}

static void test_search_edges(void)
{
    unsigned i;
    const uint32_t bins[] = {RADAR_RANGE_FIRST_BIN, RADAR_RANGE_LAST_BIN};
    for (i = 0U; i < 2U; ++i) {
        uint32_t outside = i ? bins[i] + 1U : bins[i] - 1U;
        fixture(bins[i]);
        expect(RADAR_RANGE_QUALITY_RANGE_EDGE); /* True boundary local peak. */
        range_frame.power_dbfs[outside] = -20.0f;
        expect(RADAR_RANGE_QUALITY_RANGE_EDGE); /* Stronger excluded neighbor. */
        spatial_frame.count = spatial_frame.candidate_count = 0U;
        expect(RADAR_RANGE_QUALITY_RANGE_EDGE);
        range_frame.clipped_components = 1U;
        expect(RADAR_RANGE_QUALITY_CLIPPED);
    }
}

static void test_empty_and_bad_metadata(void)
{
    unsigned i;
    fixture(64U);
    assert(radar_range_assess(NULL, &spatial_frame) == RADAR_RANGE_QUALITY_NO_DATA);
    memset(&range_frame, 0, sizeof(range_frame));
    expect(RADAR_RANGE_QUALITY_NO_DATA);
    for (i = 0U; i < 8U; ++i) {
        fixture(64U);
        if (i == 0U) range_frame.valid = 0U;
        else if (i == 1U) range_frame.session = 0U;
        else if (i == 2U) range_frame.peak_bin = RADAR_RANGE_BINS;
        else if (i == 3U) range_frame.peak_bin = RADAR_RANGE_FIRST_BIN - 1U;
        else if (i == 4U) range_frame.peak_bin = RADAR_RANGE_LAST_BIN + 1U;
        else if (i == 5U) --range_frame.hsi_records;
        else if (i == 6U) --range_frame.fft_count;
        else --range_frame.tx_records[2];
        expect(RADAR_RANGE_QUALITY_NO_DATA);
    }
}

static void test_nonpeak_and_invalid_values(void)
{
    unsigned i;
    for (i = 0U; i < 16U; ++i) {
        fixture(64U);
        switch (i) {
        case 0U: range_frame.bin_m = NAN; break;
        case 1U: range_frame.bin_m = 0.0f; break;
        case 2U: range_frame.peak_dbfs = INFINITY; break;
        case 3U: range_frame.power_dbfs[64] = NAN; break;
        case 4U: range_frame.power_dbfs[63] = INFINITY; break;
        case 5U: range_frame.power_dbfs[65] = NAN; break;
        case 6U: range_frame.power_dbfs[63] = -39.0f; break;
        case 7U: range_frame.power_dbfs[65] = -39.0f; break;
        case 8U: range_frame.power_dbfs[63] = -40.0f; break;
        case 9U: range_frame.power_dbfs[65] = -40.0f; break;
        case 10U: range_frame.peak_dbfs = -41.0f; break;
        case 11U:
            range_frame.peak_dbfs = range_frame.power_dbfs[64] = RADAR_RANGE_FLOOR_DBFS;
            range_frame.power_dbfs[63] = range_frame.power_dbfs[65] = -170.0f;
            break;
        case 12U:
            memset(range_frame.power_dbfs, 0, sizeof(range_frame.power_dbfs));
            range_frame.peak_dbfs = 0.0f; /* Constant spectrum is not a distinct peak. */
            break;
        case 13U: range_frame.bin_m = 0.5f; break;
        case 14U: range_frame.bin_m = -1.0f; break;
        default: range_frame.bin_m = INFINITY; break;
        }
        expect(RADAR_RANGE_QUALITY_UNCONFIRMED);
    }
    fixture(64U); range_frame.clipped_components = 1U;
    expect(RADAR_RANGE_QUALITY_CLIPPED);
    range_frame.clipped_components = UINT32_MAX;
    expect(RADAR_RANGE_QUALITY_CLIPPED);
    fixture(RADAR_RANGE_FIRST_BIN);
    for (i = 0U; i < RADAR_RANGE_BINS; ++i)
        range_frame.power_dbfs[i] = RADAR_RANGE_FLOOR_DBFS;
    range_frame.peak_dbfs = RADAR_RANGE_FLOOR_DBFS;
    expect(RADAR_RANGE_QUALITY_UNCONFIRMED); /* Zero-input FFT, not near-edge evidence. */
}

static void test_detector_binding_and_missing_candidates(void)
{
    unsigned i;
    fixture(64U);
    assert(radar_range_assess(&range_frame, NULL) == RADAR_RANGE_QUALITY_DETECTOR_UNAVAILABLE);
    for (i = 0U; i < 7U; ++i) {
        fixture(64U);
        if (i == 0U) spatial_frame.valid = 0U;
        else if (i == 1U) spatial_frame.error = -1;
        else if (i == 2U) ++spatial_frame.session;
        else if (i == 3U) ++spatial_frame.timestamp_us;
        else if (i == 4U) spatial_frame.count = RADAR_SPATIAL_MAX_POINTS + 1U;
        else if (i == 5U) spatial_frame.candidate_count = 0U;
        else spatial_frame.truncated_count = 1U;
        expect(RADAR_RANGE_QUALITY_DETECTOR_UNAVAILABLE);
    }
    fixture(64U);
    spatial_frame.count = spatial_frame.candidate_count = 0U;
    expect(RADAR_RANGE_QUALITY_UNCONFIRMED);
    fixture(64U); spatial_frame.points[0].range_bin = 63U;
    expect(RADAR_RANGE_QUALITY_UNCONFIRMED); /* No neighbor-bin substitution. */
    spatial_frame.points[0].range_bin = 124U;
    expect(RADAR_RANGE_QUALITY_UNCONFIRMED); /* No strongest-other-target fallback. */
    spatial_frame.count = RADAR_SPATIAL_MAX_POINTS;
    spatial_frame.candidate_count = RADAR_SPATIAL_MAX_POINTS + 2U;
    spatial_frame.truncated_count = 2U;
    for (i = 0U; i < spatial_frame.count; ++i) spatial_frame.points[i].range_bin = 100U;
    expect(RADAR_RANGE_QUALITY_UNCONFIRMED); /* Truncation is absence of confirmation. */
    spatial_frame.points[RADAR_SPATIAL_MAX_POINTS - 1U].range_bin = 64U;
    expect(RADAR_RANGE_QUALITY_QUALIFIED);
    fixture(64U); spatial_frame.points[0].snr_db = NAN;
    expect(RADAR_RANGE_QUALITY_UNCONFIRMED);
    fixture(64U); spatial_frame.points[0].power_dbfs = INFINITY;
    expect(RADAR_RANGE_QUALITY_UNCONFIRMED);
}

int main(void)
{
    test_static_and_angle_independence();
    test_search_edges();
    test_empty_and_bad_metadata();
    test_nonpeak_and_invalid_values();
    test_detector_binding_and_missing_candidates();
    assert(!strcmp(radar_range_quality_name(RADAR_RANGE_QUALITY_NO_DATA), "NO_DATA"));
    assert(!strcmp(radar_range_quality_name(RADAR_RANGE_QUALITY_RANGE_EDGE), "RANGE_EDGE"));
    assert(!strcmp(radar_range_quality_name(RADAR_RANGE_QUALITY_CLIPPED), "CLIPPED"));
    assert(!strcmp(radar_range_quality_name(RADAR_RANGE_QUALITY_UNCONFIRMED), "UNCONFIRMED"));
    assert(!strcmp(radar_range_quality_name(RADAR_RANGE_QUALITY_QUALIFIED), "QUALIFIED"));
    assert(!strcmp(radar_range_quality_name(RADAR_RANGE_QUALITY_DETECTOR_UNAVAILABLE), "DETECTOR_UNAVAILABLE"));
    assert(!strcmp(radar_range_quality_name((RadarRangeQuality)-1), "UNKNOWN"));
    puts("PASS: cached range quality, true/false boundaries, static angle-invalid peaks, source binding, clipping, finite values, no substitute peaks, truncation and immutable inputs");
    return 0;
}
