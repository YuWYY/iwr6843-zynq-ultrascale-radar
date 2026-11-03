/* SPDX-License-Identifier: MIT
 * One SYS-domain GPIO mailbox owner. Uses the CURRENT Vitis SDT XGpio driver.
 * begin/poll is non-blocking between MMIO calls. ACK means accepted/queued,
 * NOT PHY execution success, FIFO empty, DMA completion or ADC validity.
 */
#ifndef RADAR_CONTROL_H
#define RADAR_CONTROL_H
#include "xgpio.h"
#include "radar_contract.h"

#ifdef __cplusplus
extern "C" {
#endif

enum RadarControlStatus {
    RADAR_CONTROL_OK = 0,
    RADAR_CONTROL_PENDING = 1,
    RADAR_CONTROL_E_ARGUMENT = -1,
    RADAR_CONTROL_E_BSP = -2,
    RADAR_CONTROL_E_TIMER = -3,
    RADAR_CONTROL_E_TIMEOUT = -4,
    RADAR_CONTROL_E_DESYNC = -5,
    RADAR_CONTROL_E_PROTOCOL = -6,
    RADAR_CONTROL_E_REMOTE = -7,
    RADAR_CONTROL_E_ABI = -8,
    RADAR_CONTROL_E_ACTIVE = -9,
    RADAR_CONTROL_E_UNSTABLE = -10,
    RADAR_CONTROL_E_VERIFY = -11,
    RADAR_CONTROL_E_MAPPING = -12,
    RADAR_CONTROL_E_BUSY = -13,
    RADAR_CONTROL_E_LATE = -14
};

typedef struct {
    u32 raw;
    u8 ack;
    u8 error;
    u8 result;                 /* GPIO result, NOT page0F PHY result. */
    u8 index;
    u16 value;
} RadarControlReply;

typedef struct {
    XGpio gpio;
    u64 issued_us;
    u32 timeout_us;
    u32 initial_response;
    u32 last_response;         /* Last acknowledged/stable response. */
    u32 last_observed;         /* Includes stale ACK while polling. */
    u32 last_request;          /* Software-owned full write, not CH1 readback. */
    u32 polls;
    u32 submitted;
    u32 acknowledged;
    u32 remote_rejects;
    u32 timeouts;
    u32 protocol_faults;
    u32 max_ack_us;
    u32 late_acks;
    u8 ack_seed;
    u8 toggle;
    u8 pending_index;
    u8 initialized;
    u8 pending;
    u8 desynced;
    u8 abi_verified;
} RadarControl;

typedef struct {
    u32 flags;
    u32 sys_faults;
    u32 io_request;
    u32 sys_hz;
    u32 phy_status;
    u32 phy_generation;
    u32 phy_fault;
    u32 shadow[8];
} RadarControlStatusView;

/* Init changes only driver RAM, then observes response CH2 until quiet.
 * It does NOT write DATA/TRI, issue SAFE, clear faults or change IO enables.
 * Requires a running, correctly configured PL SYS clock and a sole CPU owner.
 * This application still requires the SW0 cold-start memory prerequisites. */
int radar_control_init(RadarControl *control);

/* Bounded synchronous envelope; sole runtime owner only. A late accepted ACK
 * returns E_LATE without desynchronizing; callers must not resend it. */
int radar_control_exec(RadarControl *control, u8 opcode, u8 index, u16 argument,
                       RadarControlReply *reply);
int radar_control_write_config(RadarControl *control, u8 index, u32 value);

/* Generic transport primitives for later SW2/SW3. Callers own business safety.
 * Before ABI verification only READ is permitted. Timeout/protocol failure
 * locks this object; no automatic retry, SAFE, RECOVER or hardware reset. */
int radar_control_command_begin(RadarControl *control, u8 opcode, u8 index,
                                u16 argument, u32 timeout_us);
int radar_control_command_poll(RadarControl *control, RadarControlReply *reply);

/* Blocking convenience for SHORT READ transactions only. All loops bounded.
 * read32 uses high-low-high retry for rollover; not a multi-page snapshot.
 * Dynamic snapshots need generation checks in the later snapshot service. */
int radar_control_read16(RadarControl *control, u8 page, u8 high_half, u16 *value);
int radar_control_read32(RadarControl *control, u8 page, u32 *value);
int radar_control_check_abi(RadarControl *control, u32 *actual);
int radar_control_read_status(RadarControl *control, RadarControlStatusView *view);
void radar_control_print_status(const RadarControlStatusView *view);
void radar_control_print_diagnostics(const RadarControl *control);
const char *radar_control_status_name(int status);

/* SW1 only: READ ABI/status/config, expected-invalid READs, post-error READ.
 * No IO_SET/CFG/ARM/STOP/ABORT/RECOVER/SNAPSHOT/DELAY/RECAL are submitted. */
int radar_control_run_sw1(RadarControl *control);

#ifdef __cplusplus
}
#endif
#endif
