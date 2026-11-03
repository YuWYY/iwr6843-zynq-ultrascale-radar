/* SPDX-License-Identifier: MIT */
#ifndef RADAR_SPATIAL_TYPES_H
#define RADAR_SPATIAL_TYPES_H
#include <stdint.h>
#define RADAR_SPATIAL_CHANNELS 12U
#define RADAR_SPATIAL_LOOPS 32U
#define RADAR_SPATIAL_MAX_POINTS 32U
#define RADAR_SPATIAL_MAP_RANGES 205U
#define RADAR_SPATIAL_MAP_ANGLES 121U
#define RADAR_SPATIAL_ARRAY_ID "IWR6843ISK_2P5MM_TX021_V1"
#define RADAR_POINT_AZ_VALID UINT32_C(1)
#define RADAR_POINT_EL_VALID UINT32_C(2)
#define RADAR_POINT_XYZ_VALID UINT32_C(4)
#define RADAR_POINT_DOPPLER_EDGE UINT32_C(8)
#define RADAR_POINT_LOW_COHERENCE UINT32_C(16)
#define RADAR_POINT_ANGLE_EDGE UINT32_C(32)
#define RADAR_POINT_BEARING_VALID UINT32_C(64)

typedef struct { float re, im; } RadarComplex;

typedef struct {
    /* Physical TX-major order [TX0/RX0..3, TX1/RX0..3, TX2/RX0..3]. */
    RadarComplex channel[RADAR_SPATIAL_CHANNELS];
    float range_bias_m;
    char array_id[40], cfg_sha256[65], calibration_id[48];
    uint32_t measured;
} RadarArrayCalibration;

typedef struct {
    uint32_t range_bin;
    int32_t doppler_bin;
    float range_m, velocity_mps;
    float azimuth_deg, elevation_deg, horizontal_angle_deg;
    float x_m, y_m, z_m;
    float snr_db, coherence, power_dbfs;
    uint32_t flags;
} RadarSpatialPoint;

typedef struct {
    uint32_t session, valid, calibrated;
    int32_t error;
    uint64_t timestamp_us;
    char calibration_id[48];
    uint32_t candidate_count, count, truncated_count, xyz_count;
    RadarSpatialPoint points[RADAR_SPATIAL_MAX_POINTS];
    /* Integrated, normalized 8-element horizontal beam power, dBFS.
     * Column a is projected horizontal angle (a-60) degrees. Rows 0..5
     * are excluded near range, not artificial detections. */
    float heatmap_dbfs[RADAR_SPATIAL_MAP_RANGES][RADAR_SPATIAL_MAP_ANGLES];
} RadarSpatialFrame;
#endif
