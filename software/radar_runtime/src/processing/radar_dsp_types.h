/* SPDX-License-Identifier: MIT */
#ifndef RADAR_DSP_TYPES_H
#define RADAR_DSP_TYPES_H
#include <stddef.h>
#include <stdint.h>

typedef enum { PS_REFERENCE=0, PL_BASELINE=1, PL_ENHANCED=2 } RadarDspBackend;
enum {
    RADAR_DSP_RESULT_WARMUP=1U, RADAR_DSP_RESULT_SPARSE=2U,
    RADAR_DSP_RESULT_INVALID_INPUT=4U, RADAR_DSP_RESULT_IDENTITY_RESET=8U,
    RADAR_DSP_RESULT_BASELINE_FALLBACK=64U, RADAR_DSP_RESULT_CELL_ORDER_ERROR=512U,
    RADAR_DSP_RESULT_TIME_UNTRUSTED=1024U
};
enum {
    RADAR_FRAME_HSI_VALID=1U, RADAR_FRAME_CRC_VALID=2U,
    RADAR_FRAME_TIME_TRUSTED=4U, RADAR_FRAME_CLIPPED=8U,
    RADAR_FRAME_DISCONTINUITY=16U,
    RADAR_FRAME_TIME_TRANSPORT=32U,
    /* Packed-input laboratory job, not a captured/HSI-certified radar frame.
     * Allowed only by the explicit baseline diagnostic submission path. */
    RADAR_FRAME_DIAGNOSTIC_PACKED=0x80000000U
};
/* The raw lease remains immutable until the job has quiesced. frame_id is a
 * publication identity; acquisition_id/frame_index identify the source batch.
 * timestamp_us is observation time; TIME_TRUSTED alone permits temporal use. */
typedef struct {
    const uint8_t *raw;
    uint32_t bytes, frame_id, acquisition_id, frame_index, crc32, flags;
    uint64_t timestamp_us, observed_us;
    uint32_t raw_slot,raw_lease,capture_epoch,transport_sequence,time_error_us;
    char cfg_sha256[65],profile_id[17];
} RadarFrameRef;
typedef struct {
    RadarFrameRef frame;
    RadarDspBackend backend;
    uint32_t job_id, config_epoch, task_slot, task_lease;
    uint32_t input, range_tmp, cube_out, rd_out, spectrum_out, candidates_out;
    uint32_t temporal_state, input_bytes, alpha_q31, temporal_control;
    uint32_t baseline_threshold_q16, enhanced_threshold_q16, hold_threshold_q16;
    uint32_t numeric_format,parameter_revision;
    uint32_t range_capacity,cube_capacity,rd_capacity,spectrum_capacity,candidates_capacity,temporal_capacity;
} RadarDspJob;
typedef struct {
    RadarDspBackend backend;
    uint32_t job_id, frame_id, acquisition_id, frame_index, config_epoch;
    uint32_t error, candidate_count, overflow_count, vector_count, flags;
    uint64_t timestamp_us, cycles;
    char cfg_sha256[65],profile_id[17];
    uint32_t numeric_format,parameter_revision,input_bytes;
    uint8_t valid;
} RadarDspResult;

#define RADAR_DSP_CSR_BASE UINT32_C(0xa0020000)
#define RADAR_DSP_MAGIC UINT32_C(0x52445031)
#define RADAR_DSP_ABI UINT32_C(0x00010001)
#define RADAR_DSP_NUMERIC_Q2_22 UINT32_C(1)
#define RADAR_DSP_PARAMETER_REVISION UINT32_C(1)
#define RADAR_DSP_PARAMETER_VERSION "TEMPORAL_V1_HOLD1P5_CFAR12DB"
#define RADAR_DSP_DETECTION_THRESHOLD_Q16 UINT32_C(1038676)
#define RADAR_DSP_HOLD_THRESHOLD_Q16 UINT32_C(98304)
#define RADAR_DSP_WORK_BASE UINT32_C(0x02600000)
#define RADAR_DSP_WORK_END UINT32_C(0x03600000)
#define RADAR_DSP_INPUT UINT32_C(0x02600000)
#define RADAR_DSP_RANGE UINT32_C(0x026c0000)
#define RADAR_DSP_CUBE UINT32_C(0x02780000)
#define RADAR_DSP_RD UINT32_C(0x02840000)
#define RADAR_DSP_SPECTRUM UINT32_C(0x02850000)
#define RADAR_DSP_CANDIDATES UINT32_C(0x02850800)
#define RADAR_DSP_TEMPORAL UINT32_C(0x02851000)
#define RADAR_DSP_COMPLEX_BYTES UINT32_C(786432)
#define RADAR_DSP_RD_BYTES UINT32_C(65536)
#define RADAR_DSP_SPECTRUM_BYTES UINT32_C(2048)
#define RADAR_DSP_CANDIDATES_BYTES UINT32_C(1088)
#define RADAR_DSP_TEMPORAL_BYTES UINT32_C(524352)
#define RADAR_DSP_VIEW_SLOT0 UINT32_C(0x02c00000)
#define RADAR_DSP_VIEW_SLOT1 UINT32_C(0x02c80000)
#define RADAR_DSP_JOB_SLOTS 2U
#define RADAR_DSP_JOB_SLOT_BYTES UINT32_C(0x00300000)
#define RADAR_DSP_SCENE_WORK UINT32_C(0x02d00000)
#define RADAR_DSP_WEIGHTS_WORK UINT32_C(0x02e00000)
#define RADAR_DSP_STREAM_WORK UINT32_C(0x02f00000)
#define RADAR_DSP_TELEMETRY_WORK UINT32_C(0x03000000)
#define RADAR_DSP_VIEW_SLOT_BYTES UINT32_C(0x00080000)

typedef struct {
    uint32_t count, total, truncated, error;
    uint32_t counters[12];
} RadarDspCandidateHeader;
typedef struct {
    uint32_t range, doppler;
    uint64_t power, noise_sum;
    uint32_t noise_count, flags;
} RadarDspCandidate;
_Static_assert(sizeof(RadarDspCandidateHeader)==64U,"PL candidate header ABI");
_Static_assert(sizeof(RadarDspCandidate)==32U,"PL candidate record ABI");
#endif
