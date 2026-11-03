/* SPDX-License-Identifier: MIT */
#ifndef RADAR_SESSION_H
#define RADAR_SESSION_H
#include "radar_sw3.h"
#include "radar_snapshot.h"
#include "radar_dsp.h"
typedef enum {
    RADAR_SESSION_IDLE=0, RADAR_SESSION_STOP_BEFORE, RADAR_SESSION_RECOVER_BEFORE,
    RADAR_SESSION_CONFIG, RADAR_SESSION_RECAL, RADAR_SESSION_RX_READY,
    RADAR_SESSION_DELAY, RADAR_SESSION_SHADOW, RADAR_SESSION_DMA_RUNNING,
    RADAR_SESSION_ARM_IDLE, RADAR_SESSION_START, RADAR_SESSION_RECEIVE,
    RADAR_SESSION_SNAPSHOT, RADAR_SESSION_STOP_AFTER, RADAR_SESSION_HALT,
    RADAR_SESSION_COPY, RADAR_SESSION_RESET, RADAR_SESSION_RECOVER_AFTER,
    RADAR_SESSION_DONE, RADAR_SESSION_FAILED,
    RADAR_SESSION_RECOVERY_HOLD, RADAR_SESSION_RECOVERY_DMA,
    RADAR_SESSION_RECOVERY_PL, RADAR_SESSION_RECOVERY_BOOT
} RadarSessionState;
typedef enum { RADAR_SOURCE_UNKNOWN=0,RADAR_SOURCE_STOPPED,
               RADAR_SOURCE_STARTING,RADAR_SOURCE_RUNNING,RADAR_SOURCE_HELD_RESET } RadarSourceState;
enum RadarSessionRc {
    RADAR_SESSION_OK=0, RADAR_SESSION_E_PROFILE=-201,RADAR_SESSION_E_PREREQUISITE=-202,
    RADAR_SESSION_E_OWNER=-203,RADAR_SESSION_E_GPIO=-204,RADAR_SESSION_E_UART=-205,
    RADAR_SESSION_E_PHY=-206,RADAR_SESSION_E_TIMEOUT=-207,RADAR_SESSION_E_DATA=-208,
    RADAR_SESSION_E_CANCELED=-209,RADAR_SESSION_E_DMA=-210,RADAR_SESSION_E_POWER=-211,
    RADAR_SESSION_E_RECOVERY=-212,RADAR_SESSION_E_MEMORY=-213
};
/* Independent read-only admission gates; zero means ready for an explicit
 * capture once, not proof that RF/RAW16/data-path validation has passed. */
enum RadarPreflightBlock {
    RADAR_PREFLIGHT_PROFILE=1U<<0, RADAR_PREFLIGHT_CONFIRM=1U<<1,
    RADAR_PREFLIGHT_DMA_QUAL=1U<<2, RADAR_PREFLIGHT_SW3_BUSY=1U<<3,
    RADAR_PREFLIGHT_SW2_TESTS=1U<<4, RADAR_PREFLIGHT_VERSION=1U<<5,
    RADAR_PREFLIGHT_SW2_READY=1U<<6, RADAR_PREFLIGHT_OWNER=1U<<7,
    RADAR_PREFLIGHT_CONTROL=1U<<8
};
typedef struct {
    u32 session,bytes,crc32,packet_bytes,packet_count,format,skip_pairs,startup_mode;
    u64 begin_us,armed_us,complete_us;
    /* complete_us retains its existing HALT-complete meaning. COPY timing
     * includes CRC and final CPU-store flush; ready follows teardown/recover. */
    u64 copy_begin_us,copy_complete_us,ready_us;
    RadarSnapshotData snapshot;
    RadarDmaRegisters final_dma;
    u32 dma_epoch;
    RadarQualificationKind admission_kind;
    RadarDmaPhaseResult payload_halt_result, teardown_reset_result;
    u32 payload_halt_us, teardown_reset_us;
    u8 valid;
    /* One immutable finite-batch result. Times are DMA completion observations,
     * not substituted100ms radar timestamps. The PL snapshot covers the batch. */
    u32 frame_crc32[16];
    u64 frame_observed_us[16];
    RadarTransportTime frame_transport[16];
} RadarCaptureResult;
typedef struct {
    RadarSw3 *sw3;
    RadarSw2 *sw2;
    RadarControl *control;
    RadarConsole *console;
    const RadarCaptureProfile *profile;
    RadarSessionState state,failed_phase;
    RadarSourceState source;
    RadarSnapshot snapshot;
    RadarPhyTransaction phy;
    RadarCaptureResult result;
    RadarDmaPacket copy_packet;
    RadarDmaRegisters failure_dma;
    u64 phase_us,guard_us,armed_us,begin_us,qualify_us;
    u32 next_session,active_session,config_index,shadow_index,idle_samples;
    u32 flags,sys_faults,phy_status,phy_fault,io_request;
    u32 copy_fragment,copy_fragment_offset,copy_bytes,copy_crc;
    u32 copy_frame_crc,observed_packets;
    u32 metadata_packets;
    /* Nonzero while scan consumes the previous CPU store. New acquisition
     * may fill RX, but COPY cannot overwrite this immutable lease. */
    u32 raw_store_owner,copy_wait_polls;
    u8 metadata_capable;
    u32 captures_passed,captures_failed,recoveries,phase_polls;
    u32 export_index,export_offset;
    u32 preflight_blockers,admission_epoch,normal_halt_passes;
    /* Optional SDK restart of the exact preceding successful configuration.
     * Cached identities are expected compiled/CLI identities, not flash readback. */
    const RadarCaptureProfile *reuse_profile;
    u32 reuse_reset_epoch,active_reset_epoch;
    u32 full_config_completed,reuse_completed;
    char reuse_profile_id[17],reuse_cfg_sha256[65],reuse_firmware_sha256[65];
    char reuse_version[128];
    u8 reuse_enabled,reuse_valid,active_reuse;
    RadarQualificationKind admission_kind;
    int error,native_error,cleanup_error;
    u8 owns,confirmed,copy_lease,exporting,reported_failure;
} RadarSession;
void radar_session_init(RadarSession *s,RadarSw3 *sw3,RadarSw2 *sw2,
                         RadarControl *control,RadarConsole *console);
/* When owns!=0, this is the ONLY poller of DMA and UART1. */
void radar_session_poll(RadarSession *s);
/* Read-only safe rendering gap between acknowledged config commands. */
int radar_session_display_idle(const RadarSession *s);
u32 radar_session_preflight(RadarSession *s);
void radar_session_print_preflight(RadarSession *s);
/* Returns 1 if handled/blocked, 0 if safely delegable to SW3/SW2. */
int radar_session_command(RadarSession *s,const char *command);
int radar_session_start(RadarSession *s);
int radar_session_lease_result(RadarSession *s,RadarCaptureResult *copy);
int radar_session_release_raw(RadarSession *s,u32 owner);
int radar_session_recover(RadarSession *s);
void radar_session_cancel(RadarSession *s);
/* Downstream HSI/CRC/processing failures invalidate restart evidence without
 * touching hardware or changing the user's explicit reuse mode. */
void radar_session_invalidate_config(RadarSession *s);
const char *radar_session_state_name(RadarSessionState state);
const char *radar_session_error_name(int error);
void radar_session_print_status(RadarSession *s);
/* ASCII/CRC export uses the separate immutable CPU store, never DMA buffers. */
void radar_export_poll(RadarSession *s);
#endif
