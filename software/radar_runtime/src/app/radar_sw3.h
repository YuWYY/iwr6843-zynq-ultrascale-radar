/* SPDX-License-Identifier: MIT */
#ifndef RADAR_SW3_H
#define RADAR_SW3_H
#include "radar_dma.h"
#include "radar_sw2.h"
typedef enum {
    SW3_IDLE=0, SW3_WAIT_RUN, SW3_OBSERVE, SW3_WAIT_HALT,
    SW3_WAIT_RESET, SW3_RESET_ONLY, SW3_REPEAT_WAIT
} RadarSw3Job;
/* Historical pass counters are NOT admission permissions. This one-use
 * permission is issued only after an entire requested batch completes. */
typedef enum {
    RADAR_QUAL_NONE=0, RADAR_QUAL_RESET_CONTROL, RADAR_QUAL_RUN_HALT_RESET
} RadarQualificationKind;
typedef enum {
    RADAR_QUAL_NOT_TESTED=0, RADAR_QUAL_VALID, RADAR_QUAL_NEW_TEST,
    RADAR_QUAL_TEST_FAILED, RADAR_QUAL_EXPLICIT_RESET, RADAR_QUAL_STALE_EPOCH,
    RADAR_QUAL_DMA_NOT_READY, RADAR_QUAL_CONSUMED, RADAR_QUAL_SESSION_FAILED,
    RADAR_QUAL_CANCELED
} RadarQualificationReason;
typedef struct {
    RadarDma dma;
    RadarSw2 *sw2;
    RadarControl *control;
    RadarConsole *console;
    RadarSw3Job job;
    u64 phase_us, guard_us;
    u32 guard_flags, guard_io, checks_passed, checks_failed;
    u32 direct_reset_passes, reset_only_passes, batch_total, batch_done;
    u32 observed_completed_bds, observed_completed_packets, expected_tail;
    u32 qualification_epoch, qualification_reset_count;
    RadarQualificationKind qualification_kind;
    RadarQualificationReason qualification_reason;
    int check_error;
    u8 canceled, run_observed, direct_reset, control_qualified, reset_qualified;
} RadarSw3;
void radar_sw3_init(RadarSw3 *s,RadarSw2 *sw2,RadarControl *control,RadarConsole *console);
/* Called only when SW4 has NOT claimed the runtime. Owns the one DMA poll. */
void radar_sw3_poll(RadarSw3 *s);
int radar_sw3_command(RadarSw3 *s,const char *command);
void radar_sw3_print_dma(RadarSw3 *s);
/* Read-only hardware validation, with fail-closed invalidation of stale local
 * permissions. Never starts/stops/resets DMA, touches payload, or enables RF. */
int radar_sw3_capture_ready(RadarSw3 *s);
void radar_sw3_revoke_qualification(RadarSw3 *s, RadarQualificationReason reason);
/* Single cooperative owner must already have claimed SW2. Consumes before
 * first sensorStop/config/ARM. Success/failure never auto-renews permission. */
int radar_sw3_consume_qualification(RadarSw3 *s);
const char *radar_sw3_qualification_name(RadarQualificationKind kind);
const char *radar_sw3_qualification_reason(RadarQualificationReason reason);
void radar_sw3_print_qualification(RadarSw3 *s);
#endif
