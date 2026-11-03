/* SPDX-License-Identifier: MIT
 * Capture contract is generated from the ACTUAL bin/cfg plus a reviewed
 * evidence file. Hashes are expected identities, not readback of flashed code.
 */
#ifndef RADAR_CAPTURE_PROFILE_H
#define RADAR_CAPTURE_PROFILE_H
#include "radar_contract.h"
#include "xil_types.h"
#define RADAR_CAPTURE_PROFILE_SCHEMA 1U
#define RADAR_CAPTURE_APPROVAL_MASK UINT32_C(0x000000ff)
#define RADAR_CAPTURE_TRIAL_APPROVAL_MASK UINT32_C(0x00000083)
#define RADAR_CAPTURE_QUALIFIED 0U
#define RADAR_CAPTURE_FIRST_PACKET_TEST 1U
#define RADAR_CAPTURE_FINITE_FRAME_TEST 2U
#define RADAR_CAPTURE_STREAM_FRAME_TEST 3U
#define RADAR_CAPTURE_SOURCE_UART UINT32_C(1)
#define RADAR_CAPTURE_SOURCE_WIRE UINT32_C(2)
#define RADAR_CAPTURE_SOURCE_PREAMBLE UINT32_C(4)
#define RADAR_CAPTURE_SOURCE_REVIEW_MASK UINT32_C(7)
#define RADAR_CAPTURE_MAX_COMMANDS 96U
#define RADAR_CAPTURE_MAX_LINE 239U
#define RADAR_CAPTURE_START_IDLE_CLOCK 1U
#define RADAR_CAPTURE_START_REVIEWED_PREAMBLE 2U

typedef enum { RADAR_CAPTURE_CMD_STOP=0, RADAR_CAPTURE_CMD_CONFIG,
               RADAR_CAPTURE_CMD_START, RADAR_CAPTURE_CMD_START_REUSE } RadarCaptureCommand;
typedef struct {
    u32 schema, approvals;
    /* A bounded trial retains unapproved physical timing/wire evidence bits.
     * Source-review flags are file-backed compatibility reviews, not measurements. */
    u32 qualification, source_review_flags;
    const char *name, *profile_id, *firmware_sha256, *cfg_sha256, *review_sha256;
    const char *version_substring;
    u32 lanes, lane_mbps, word_bits, format_flags;
    u32 packet_bytes, packet_count, skip_pairs;
    u32 startup_mode, receive_timeout_us, snapshot_timeout_us;
    u32 use_delay, taps[3]; /* absolute counts; never apply below actual baseline */
    u32 command_count;
    const char *const *commands; /* configuration ONLY: no implicit start */
    const char *start_command, *stop_command;
} RadarCaptureProfile;
typedef struct {
    u32 source_frames, hsi_header_enabled, chirps_per_frame, samples_per_rx, rx_mask;
    u32 adc_bytes_per_chirp, expected_header_bytes, wire_bytes_per_frame;
    u32 tx_count, tx_mask, loops, tx_order[3];
} RadarFiniteGeometry;
const RadarCaptureProfile *radar_capture_profile_get(void);
const RadarCaptureProfile *radar_capture_profile_batch(u32 frames);
const RadarCaptureProfile *radar_capture_profile_stream(u32 frames);
int radar_capture_profile_stream_geometry(const RadarCaptureProfile *,RadarFiniteGeometry *);
int radar_capture_profile_supported(const RadarCaptureProfile *p);
/* Pure validation. Returns zero ONLY for a structurally valid, approved
 * contract; does not claim to measure voltages/timing or identify live bin. */
int radar_capture_profile_validate(const RadarCaptureProfile *p);
int radar_capture_profile_version_matches(const RadarCaptureProfile *p,const char *version);
/* Frozen SDK 3.5 finite-frame geometry; returns nonzero for any unsupported
 * command/value/duplicate or mismatched transport size. No live ADC proof. */
int radar_capture_profile_finite_geometry(const RadarCaptureProfile *p,RadarFiniteGeometry *out);
const char *radar_capture_profile_command(const RadarCaptureProfile *p,
                                         RadarCaptureCommand kind,u32 index);
#endif
