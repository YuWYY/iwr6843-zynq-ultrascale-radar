/* SPDX-License-Identifier: MIT */
#ifndef RADAR_SCENE_TYPES_H
#define RADAR_SCENE_TYPES_H
#include <stdint.h>
#define RADAR_SCENE_MAX_TRACKS 8U
#define RADAR_SCENE_MAX_WALLS 4U
#define RADAR_SCENE_MAX_BEARINGS 32U
#define RADAR_SCENE_MAX_REFLECTIONS 8U
#define RADAR_SCENE_TRAIL_POINTS 32U
#define RADAR_SCENE_RULES_ID "RULES_V1"
#define RADAR_SCENE_WORKSPACE_LIMIT (256U * 1024U)

enum RadarSceneLabel { RADAR_SCENE_UNKNOWN, RADAR_SCENE_MOVING_OBJECT,
    RADAR_SCENE_STATIC_REFLECTOR, RADAR_SCENE_HUMAN_CANDIDATE };
enum RadarSceneRule { RADAR_SCENE_RULE_EXTENT=1U, RADAR_SCENE_RULE_MOTION=2U,
    RADAR_SCENE_RULE_PERSISTENCE=4U, RADAR_SCENE_RULE_NONRIGID=8U };
enum RadarSceneFlag { RADAR_SCENE_TIME_UNTRUSTED=1U, RADAR_SCENE_MAP_WARMUP=2U,
    RADAR_SCENE_MAP_RESET=4U, RADAR_SCENE_INPUT_TRUNCATED=8U,
    RADAR_SCENE_CAPACITY=16U, RADAR_SCENE_QUALITY_REJECTED=32U,
    RADAR_SCENE_AMBIGUOUS_ASSOCIATION=64U };
enum RadarSceneResetReason { RADAR_SCENE_RESET_START=0U,
    RADAR_SCENE_RESET_CONTEXT=1U, RADAR_SCENE_RESET_TIME=2U,
    RADAR_SCENE_RESET_INSTALLATION=3U, RADAR_SCENE_RESET_MAP_CHANGE=4U,
    RADAR_SCENE_RESET_CANCEL=5U };
typedef struct {
    uint32_t capture_epoch, installation_epoch, frame_id, time_valid;
    uint64_t timestamp_us;
    char cfg_sha256[65], calibration_id[48];
    uint32_t frame_flags; /* Original RADAR_FRAME_* certification, not labels. */
} RadarSceneContext;
enum RadarReflectionState { RADAR_REFLECTION_UNKNOWN=0U,
    RADAR_REFLECTION_TENTATIVE=1U, RADAR_REFLECTION_STABLE=2U,
    RADAR_REFLECTION_STALE=3U, RADAR_REFLECTION_AMBIGUOUS=4U };
enum RadarReflectionFlag { RADAR_REFLECTION_MEASURED=1U,
    RADAR_REFLECTION_U_VALID=2U, RADAR_REFLECTION_INPUT_LIMITED=4U };
enum RadarSceneReject { RADAR_SCENE_REJECT_INTEGRITY=1U,
    RADAR_SCENE_REJECT_TIME=2U, RADAR_SCENE_REJECT_WARMUP=4U,
    RADAR_SCENE_REJECT_NO_FRESH=8U, RADAR_SCENE_REJECT_SUPPORT=16U,
    RADAR_SCENE_REJECT_POINT_RESPONSE=32U, RADAR_SCENE_REJECT_SPAN=64U,
    RADAR_SCENE_REJECT_RESIDUAL=128U, RADAR_SCENE_REJECT_OCCUPANCY=256U,
    RADAR_SCENE_REJECT_GAP=512U, RADAR_SCENE_REJECT_ASSOCIATION=1024U,
    RADAR_SCENE_REJECT_CAPACITY=2048U, RADAR_SCENE_REJECT_PROJECTION_ONLY=4096U,
    RADAR_SCENE_REJECT_CLIPPED=8192U, RADAR_SCENE_REJECT_BUDGET=16384U };
/* Fixed 64-byte snapshot. range_m is latest observed LOS, median/MAD are
 * measured-window summaries. u is a direction cosine, never an XY/Z fix. */
typedef struct {
    uint32_t id,state,flags,support_buckets;
    uint64_t first_observed_us,last_observed_us;
    float range_m,range_median_m,range_mad_m,u,power_dbfs,snr_db;
    uint32_t sample_count,reject_flags;
} RadarStableReflection;
/* Fixed 128-byte diagnostic snapshot for the versioned recorder contract. */
typedef struct {
    uint32_t version,reject_flags,valid_buckets,required_buckets,map_elapsed_ms;
    uint32_t radial_candidates,persistent_cells,independent_groups,point_like_groups;
    uint32_t candidate_overflow,fit_trials,best_support_cells,best_support_groups;
    uint32_t stable_count,tentative_count,stale_count,ambiguous_count,range_only_count;
    uint32_t rejected_points,capacity_count,processing_us,budget_overruns,reserved0,reserved1;
    float best_span_m,best_residual_m,best_occupancy,best_temporal_support;
    float best_single_point_ratio,range_mad_limit_m,association_range_m,association_u;
} RadarSceneDiagnostics;
typedef struct { float x_m,y_m; uint64_t timestamp_us; } RadarSceneTrailPoint;
typedef struct {
    uint32_t id, confirmed, measured, predicted, label, rule_bits, rule_score;
    uint32_t point_count, trail_count;
    uint64_t first_measured_us,last_measured_us;
    float x_m,y_m,vx_mps,vy_mps,extent_m,snr_db,position_variance_m2;
    float shape_variation_m,radial_spread_mps,motion_speed_mps;
    /* No Z is invented by a two-dimensional tracker. Heights remain in the
     * original spatial points; trajectories contain only real XY measurements. */
    RadarSceneTrailPoint trail[RADAR_SCENE_TRAIL_POINTS];
} RadarSceneTrack;
typedef struct {
    float x0_m,y0_m,x1_m,y1_m,residual_m,support_fraction;
    uint32_t cells;
    uint64_t latest_observed_us;
    /* A line in the horizontal-projection reflection map, not a reconstructed
     * wall plane or a ground-truth XY position of unknown-height scatterers. */
} RadarSceneWall;
typedef struct {
    float range_m, horizontal_angle_deg, snr_db;
    uint32_t flags;
} RadarSceneBearing;
typedef struct {
    uint32_t valid,session,frame_id,capture_epoch,installation_epoch,calibrated;
    uint64_t timestamp_us;
    uint32_t flags,reset_reason,track_count,confirmed_count,wall_count,bearing_count;
    uint32_t raw_points,rejected_points,input_truncated_count,omitted_tracks;
    uint32_t map_observed_buckets,map_required_buckets,rule_version;
    char calibration_id[48];
    RadarSceneTrack tracks[RADAR_SCENE_MAX_TRACKS];
    RadarSceneWall walls[RADAR_SCENE_MAX_WALLS];
    RadarSceneBearing bearings[RADAR_SCENE_MAX_BEARINGS];
    uint32_t reflection_count;
    RadarSceneDiagnostics diagnostics;
    RadarStableReflection reflections[RADAR_SCENE_MAX_REFLECTIONS];
} RadarSceneFrame;
#endif
