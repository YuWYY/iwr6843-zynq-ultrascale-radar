/* SPDX-License-Identifier: MIT */
#ifndef RADAR_VIEW_TYPES_H
#define RADAR_VIEW_TYPES_H
#include <stdint.h>
#include "radar_freshness.h"

typedef enum {
    RADAR_VIEW_OVERVIEW = 0, RADAR_VIEW_AZIMUTH,
    RADAR_VIEW_TOP, RADAR_VIEW_SPACE, RADAR_VIEW_COUNT
} RadarViewPage;

/* Copied software observations only. Building or painting this structure must
 * never issue a radar command or probe hardware. last_result_valid applies to
 * the retained result fields, not to the entire history or current IO state. */
typedef struct {
    uint64_t sampled_us;
    /* Age since software observed this complete capture; not a subtraction
     * between the PS uptime and the PL-relative sample timestamp. */
    uint64_t frame_age_us;
    uint32_t frame_age_valid;
    /* Same immutable frame publication; AGE AT DRAW does not renew its cap. */
    RadarSampleFreshness freshness;
    char capture_state[32], source_state[24], profile_id[24];
    uint32_t captures_passed, captures_failed;
    int32_t capture_error, native_error, cleanup_error;
    uint32_t last_result_valid, last_session, last_bytes, last_crc32;
    uint32_t last_dma_control, last_dma_status;
    uint32_t sys_faults, phy_status, phy_fault;
    uint32_t control_timeouts, control_protocol_faults;
    uint32_t tx_count, rx_count, chirps, adc_samples;
    /* Display fills these from its existing cached state before painting. */
    uint32_t dp_active, dp_isr_errors, dp_eisr_errors;
    uint32_t data_presented_revision, view_presented_revision;
    char dp_state[56];
} RadarViewDiagnostics;

#endif
