/* SPDX-License-Identifier: MIT
 * SW4.1 finite acquisition supervisor. Single cooperative owner of GPIO/UART/SG.
 * No IRQ/RTOS, second GPIO or second data source. A transport packet is NOT a
 * chirp/frame. RF/config actions require compiled, reviewed actual bin/cfg plus
 * explicit local confirmation and SW3/SW2 prerequisites from this boot.
 */
#include "radar_session.h"
#include "radar_crc32.h"
#include "radar_memory.h"
#include "radar_hw.h"
#include "xil_cache.h"
#include <string.h>
#define SAY(s,...) ((void)radar_console_printf((s)->console,__VA_ARGS__))
#define PG (RADAR_FLAG_PG|RADAR_FLAG_PG_QUALIFIED)
#define COPY_QUANTUM 4096U
#define CONTROL_WAIT_US 100000U
#define PHASE_POLL_BOUND 10000000U

static void invalidate_reuse(RadarSession *s)
{
    s->reuse_valid=0U;s->reuse_profile=NULL;
}
void radar_session_invalidate_config(RadarSession *s)
{
    if (s) { invalidate_reuse(s); }
}
static void check_reuse_epoch(RadarSession *s)
{
    /* Normal teardown disables UART and the next version query temporarily
     * clears version_valid. Neither operation changes radar configuration.
     * NRST, faults, or a new compiled profile do invalidate it. */
    if (s->reuse_valid && (s->reuse_profile!=s->profile ||
        s->reuse_reset_epoch!=s->sw2->cli.reset_epoch || s->sw2->cli.needs_reset ||
        s->sw2->fatal_control || s->control->desynced ||
        s->sw2->state==SW2_FAULT || s->sw2->state==SW2_RESET_HOLD ||
        s->sw2->state==SW2_RESET_BOOT || (s->sw2->flags&PG)!=PG ||
        (s->sw2->flags&RADAR_FLAG_POWER_LOST) ||
        !(s->sw2->flags&RADAR_FLAG_NERR_N))) { invalidate_reuse(s); }
}
static int reuse_matches(const RadarSession *s)
{
    const RadarCaptureProfile *p=s->profile;
    const RadarCli *c=&s->sw2->cli;
    return s->reuse_valid && s->reuse_profile==p &&
        s->reuse_reset_epoch==c->reset_epoch && !c->needs_reset &&
        c->version_valid && c->version_epoch==c->reset_epoch &&
        radar_capture_profile_validate(p)==0 &&
        radar_capture_profile_version_matches(p,c->last_version) &&
        strcmp(s->reuse_profile_id,p->profile_id)==0 &&
        strcmp(s->reuse_cfg_sha256,p->cfg_sha256)==0 &&
        strcmp(s->reuse_firmware_sha256,p->firmware_sha256)==0 &&
        strcmp(s->reuse_version,c->last_version)==0;
}
static void remember_completed_config(RadarSession *s)
{
    const RadarCaptureProfile *p=s->profile;
    const RadarCli *c=&s->sw2->cli;
    invalidate_reuse(s);
    if (!s->reuse_enabled || s->source!=RADAR_SOURCE_STOPPED ||
        s->active_reset_epoch!=c->reset_epoch || c->needs_reset ||
        !c->version_valid || c->version_epoch!=c->reset_epoch ||
        !radar_capture_profile_version_matches(p,c->last_version)) { return; }
    s->reuse_profile=p;s->reuse_reset_epoch=c->reset_epoch;
    memcpy(s->reuse_profile_id,p->profile_id,sizeof s->reuse_profile_id);
    memcpy(s->reuse_cfg_sha256,p->cfg_sha256,sizeof s->reuse_cfg_sha256);
    memcpy(s->reuse_firmware_sha256,p->firmware_sha256,sizeof s->reuse_firmware_sha256);
    memcpy(s->reuse_version,c->last_version,sizeof s->reuse_version);
    s->reuse_valid=1U;
}
static void print_reuse(RadarSession *s)
{
    check_reuse_epoch(s);
    SAY(s,"[CAPTURE REUSE] enabled=%u cache=%u last=%s full_completed=%u reuse_completed=%u reset_epoch=%u; expected config identity, not flash readback\r\n",
        (unsigned)s->reuse_enabled,(unsigned)reuse_matches(s),s->active_reuse?"REUSE":"FULL",
        (unsigned)s->full_config_completed,(unsigned)s->reuse_completed,(unsigned)s->sw2->cli.reset_epoch);
}
static void enter(RadarSession *s,RadarSessionState state,u64 now)
{
    s->state=state;s->phase_us=now;s->phase_polls=0U;
}
static int read_page(RadarSession *s,u8 page,u32 *value)
{
    return radar_control_read32(s->control,page,value);
}
static int health(RadarSession *s)
{
    int rc;
    u32 want;
    rc=read_page(s,RADAR_PAGE_FLAGS,&s->flags);if(rc){return rc;}
    rc=read_page(s,RADAR_PAGE_IO_REQUEST,&s->io_request);if(rc){return rc;}
    rc=read_page(s,RADAR_PAGE_SYS_FAULTS,&s->sys_faults);if(rc){return rc;}
    rc=read_page(s,RADAR_PAGE_PHY_STATUS,&s->phy_status);if(rc){return rc;}
    rc=read_page(s,RADAR_PAGE_PHY_FAULT,&s->phy_fault);if(rc){return rc;}
    if ((s->flags&PG)!=PG||(s->flags&RADAR_FLAG_POWER_LOST)) { return RADAR_SESSION_E_POWER; }
    want=s->sw2->expected_io;
    if (s->io_request!=want) { return RADAR_SESSION_E_OWNER; }
    if (want==3U && ((s->flags&RADAR_SW1_OUTPUT_MASK)!=(RADAR_FLAG_UART_OE|RADAR_FLAG_SYNC_OE))) { return RADAR_SESSION_E_OWNER; }
    if (want==5U && (s->flags&RADAR_SW1_OUTPUT_MASK)!=RADAR_FLAG_NRST_LOW) { return RADAR_SESSION_E_OWNER; }
    if (want==0U && (s->flags&RADAR_SW1_OUTPUT_MASK)) { return RADAR_SESSION_E_OWNER; }
    if (s->state!=RADAR_SESSION_RECOVERY_HOLD && s->state!=RADAR_SESSION_RECOVERY_DMA &&
        s->state!=RADAR_SESSION_RECOVERY_PL && s->state!=RADAR_SESSION_RECOVERY_BOOT &&
        !(s->flags&RADAR_FLAG_NERR_N)) { return RADAR_SESSION_E_POWER; }
    /* Initial sticky faults may only be cleared after acknowledged sensorStop
     * and verified DMA quiescence. Explicit RECAL owns its old PHY fault. */
    if (s->state>=RADAR_SESSION_CONFIG && s->state<=RADAR_SESSION_COPY) {
        if (s->sys_faults || (s->flags&RADAR_FLAG_RX_FAULT)) { return RADAR_SESSION_E_DATA; }
    }
    if (s->state>=RADAR_SESSION_RX_READY && s->state<=RADAR_SESSION_SNAPSHOT && s->phy_fault) { return RADAR_SESSION_E_PHY; }
    if (s->state>=RADAR_SESSION_CONFIG && s->state<=RADAR_SESSION_SHADOW &&
        (s->flags&(RADAR_FLAG_SESSION_OPEN|RADAR_FLAG_ARM_PENDING|RADAR_FLAG_CMD_PENDING))) { return RADAR_SESSION_E_OWNER; }
    return 0;
}
static int exec(RadarSession *s,u8 opcode,u16 argument)
{
    return radar_control_exec(s->control,opcode,0U,argument,NULL);
}
static void failure(RadarSession *s,int error,int native)
{
    RadarDma *d=&s->sw3->dma;
    invalidate_reuse(s);
    if (s->state==RADAR_SESSION_FAILED) { return; }
    s->failed_phase=s->state;s->error=error;s->native_error=native;
    s->result.valid=0U;s->exporting=0U;
    radar_sw3_revoke_qualification(s->sw3,RADAR_QUAL_SESSION_FAILED);
    ++s->captures_failed;
    (void)radar_dma_registers(d,&s->failure_dma);
    /* Cancellation never means sensorStop. If START may have been sent, the
     * producer remains UNKNOWN until a real stop ACK or verified reset hold. */
    if (s->source==RADAR_SOURCE_STARTING || s->source==RADAR_SOURCE_RUNNING ||
        s->source==RADAR_SOURCE_HELD_RESET) { s->source=RADAR_SOURCE_UNKNOWN; }
    /* IO_OFF below releases NRST: a failed recovery must not keep claiming
     * HELD_RESET after the physical hold is removed. */
    radar_cli_cancel(&s->sw2->cli);
    if (!s->control->desynced && !s->control->pending) {
        int rc=exec(s,RADAR_OP_ABORT,0U);
        if (rc) { s->cleanup_error=rc; }
    }
    {
        int rc=radar_sw2_capture_io(s->sw2,0U);
        if (rc) { s->cleanup_error=rc; }
    }
    if (s->copy_lease) {
        int rc=radar_dma_release(d,&s->copy_packet);
        if (rc==0) { s->copy_lease=0U; }
        else { s->cleanup_error=rc; }
    }
    if (d->configured && !d->lease_active && d->state!=RADAR_DMA_RESETTING &&
        d->state!=RADAR_DMA_LOCKED && d->state!=RADAR_DMA_HALT_TIMEOUT &&
        d->state!=RADAR_DMA_HALTED && d->state!=RADAR_DMA_HALTING) {
        int rc=radar_dma_stop_begin(d);
        if (rc<0) { s->cleanup_error=rc; }
    }
    enter(s,RADAR_SESSION_FAILED,radar_hw_now_us());
    SAY(s,"RESULT: FAIL SW4 phase=%s reason=%s native=%d cleanup=%d; data NOT valid\r\n",
        radar_session_state_name(s->failed_phase),radar_session_error_name(error),native,s->cleanup_error);
    SAY(s,"[SW4 SAFETY] Local outputs off requested; RF/source stop is NOT implied. source=%u DMA=%s. No RECOVER/free/reset issued blindly.\r\n",
        (unsigned)s->source,radar_dma_state_name(d->state));
    SAY(s,"[NEXT] capture status, dma status/trace. capture recover explicitly authorizes NRST hold + ONE DMA reset. RESET_LOCKED/mailbox loss needs full target initialization.\r\n");
}
static int recover_pl(RadarSession *s)
{
    if ((s->source!=RADAR_SOURCE_STOPPED && s->source!=RADAR_SOURCE_HELD_RESET) ||
        !radar_dma_is_quiescent(&s->sw3->dma) || s->sw3->dma.lease_active ||
        s->sw3->dma.ring->HwCnt || s->sw3->dma.ring->PostCnt || s->sw3->dma.ring->PreCnt) { return -1; }
    return exec(s,RADAR_OP_RECOVER,0xC1EAU);
}
static int recover_complete(RadarSession *s)
{
    int rc=read_page(s,RADAR_PAGE_FLAGS,&s->flags);
    if (rc) { return rc; }
    if (s->flags&(RADAR_FLAG_RECOVERING|RADAR_FLAG_SESSION_OPEN|RADAR_FLAG_CMD_PENDING|
        RADAR_FLAG_ARM_PENDING|RADAR_FLAG_SNAPSHOT_PENDING|RADAR_FLAG_PHY_BUSY)) { return 1; }
    if (!(s->flags&RADAR_FLAG_DMA_RESETN)) { return 1; }
    rc=read_page(s,RADAR_PAGE_SYS_FAULTS,&s->sys_faults);
    if (rc) { return rc; }
    return s->sys_faults?-1:0;
}
static int start_cli(RadarSession *s,RadarCaptureCommand kind,u32 index,u64 now)
{
    if (kind==RADAR_CAPTURE_CMD_START_REUSE &&
        (!s->active_reuse || !reuse_matches(s) || s->config_index!=0U)) {
        return RADAR_CLI_E_PROFILE;
    }
    return radar_cli_start_capture(&s->sw2->cli,s->profile,kind,index,now);
}
static int cli_result(RadarSession *s)
{
    int result;
    if (!radar_cli_take_result(&s->sw2->cli,&result)) { return 1; }
    return result==0?0:result;
}
static int write_shadow(RadarSession *s,u8 index,u32 value)
{
    return radar_control_write_config(s->control,index,value);
}
void radar_session_init(RadarSession *s,RadarSw3 *sw3,RadarSw2 *sw2,RadarControl *control,RadarConsole *console)
{
    memset(s,0,sizeof(*s));s->sw3=sw3;s->sw2=sw2;s->control=control;s->console=console;
    s->profile=radar_capture_profile_get();s->next_session=1U;s->state=RADAR_SESSION_IDLE;
    SAY(s,"[SW4.1] capture preflight | capture profile | capture confirm ID | capture once | capture status | capture export | capture clear | capture recover\r\n");
    SAY(s,"[SW4] Default profile is unqualified unless generated from actual bin/cfg/review. Init starts no RF/DMA; authorized deployment autoboot may start after native admission.\r\n");
}
int radar_session_display_idle(const RadarSession *s)
{
    /* Rendering is allowed only BETWEEN fully acknowledged config commands.
     * No UART reply, ARM, running RX or mailbox deadline is held here. */
    return s && s->owns && s->state==RADAR_SESSION_CONFIG &&
        s->source==RADAR_SOURCE_STOPPED && s->config_index<s->profile->command_count &&
        s->sw2->cli.state==RADAR_CLI_IDLE && !s->sw2->cli.terminal &&
        !s->sw2->cli.needs_reset && !s->sw2->fatal_control && !s->control->pending &&
        s->sw3->dma.state==RADAR_DMA_READY;
}
u32 radar_session_preflight(RadarSession *s)
{
    u32 blocked=0U;
    if (radar_capture_profile_validate(s->profile)!=0) { blocked|=RADAR_PREFLIGHT_PROFILE; }
    if (!s->confirmed) { blocked|=RADAR_PREFLIGHT_CONFIRM; }
    if (!radar_sw3_capture_ready(s->sw3)) { blocked|=RADAR_PREFLIGHT_DMA_QUAL; }
    if (s->sw3->job!=SW3_IDLE) { blocked|=RADAR_PREFLIGHT_SW3_BUSY; }
    if (!s->sw2->check10_passes || !s->sw2->reset3_passes) { blocked|=RADAR_PREFLIGHT_SW2_TESTS; }
    if (!s->sw2->cli.version_valid || s->sw2->cli.version_epoch!=s->sw2->cli.reset_epoch ||
        !radar_capture_profile_version_matches(s->profile,s->sw2->cli.last_version)) {
        blocked|=RADAR_PREFLIGHT_VERSION;
    }
    if (s->owns || s->exporting || s->result.valid || !s->next_session ||
        (s->state!=RADAR_SESSION_IDLE && s->state!=RADAR_SESSION_DONE)) {
        blocked|=RADAR_PREFLIGHT_OWNER;
    }
    if (s->control->desynced || s->control->pending || !s->control->abi_verified ||
        s->sw2->fatal_control) { blocked|=RADAR_PREFLIGHT_CONTROL; }
    /* Never perform a competing mailbox transaction during an owned session.
     * This check only reads IO status; no enabling, reset, UART TX or claim. */
    if ((blocked&(RADAR_PREFLIGHT_CONTROL|RADAR_PREFLIGHT_OWNER|RADAR_PREFLIGHT_SW3_BUSY)) ||
        radar_sw2_capture_check(s->sw2)!=0) { blocked|=RADAR_PREFLIGHT_SW2_READY; }
    s->preflight_blockers=blocked;
    return blocked;
}
void radar_session_print_preflight(RadarSession *s)
{
    static const char *const gates[] = {
        "GENERATED_PROFILE", "USER_CONFIRM", "DMA_EPOCH_PERMISSION", "SW3_IDLE",
        "SW2_CHECK10_RESET3", "CURRENT_VERSION", "SW2_ONLINE_POWER_IO",
        "IDLE_EMPTY_RESULT", "GPIO_ABI_OWNER"
    };
    u32 i,blocked=radar_session_preflight(s);
    SAY(s,"[PREFLIGHT] %s blockers=%08x; READ_ONLY, no UART TX/ARM/reset/RF; does not consume permission\r\n",
        blocked?"BLOCKED":"READY",(unsigned)blocked);
    for (i=0U;i<sizeof(gates)/sizeof(gates[0]);++i) {
        SAY(s,"[GATE] %s=%s\r\n",gates[i],(blocked&(1U<<i))?"BLOCK":"PASS");
    }
    radar_sw3_print_qualification(s->sw3);
}
int radar_session_lease_result(RadarSession *s,RadarCaptureResult *copy)
{
    if(!s || !copy || copy==&s->result || s->owns || s->exporting || s->raw_store_owner ||
       s->state!=RADAR_SESSION_DONE || !s->result.valid)return RADAR_SESSION_E_OWNER;
    *copy=s->result;s->raw_store_owner=s->result.session;
    memset(&s->result,0,sizeof s->result);s->state=RADAR_SESSION_IDLE;return 0;
}
int radar_session_release_raw(RadarSession *s,u32 owner)
{
    if(!s || !owner || s->raw_store_owner!=owner)return RADAR_SESSION_E_OWNER;
    s->raw_store_owner=0U;return 0;
}
int radar_session_start(RadarSession *s)
{
    int rc;RadarDma *d=&s->sw3->dma;
    check_reuse_epoch(s);
    if (s->owns || s->exporting || s->result.valid ||
        (s->state!=RADAR_SESSION_IDLE&&s->state!=RADAR_SESSION_DONE)) { return RADAR_SESSION_E_OWNER; }
    if (s->profile->qualification==RADAR_CAPTURE_STREAM_FRAME_TEST || radar_capture_profile_validate(s->profile)||!s->confirmed) { return RADAR_SESSION_E_PROFILE; }
    if (radar_session_preflight(s)!=0U) { return RADAR_SESSION_E_PREREQUISITE; }
    if (!s->next_session || d->state!=RADAR_DMA_READY || !d->reset_confirmed || !radar_dma_is_quiescent(d) ||
        d->packet_queued || d->ring->FreeCnt!=(int)RADAR_RX_BD_COUNT) { return RADAR_SESSION_E_OWNER; }
    rc=radar_sw2_capture_claim(s->sw2);if(rc){return RADAR_SESSION_E_OWNER;}
    s->active_reuse=(u8)(s->reuse_enabled && s->source==RADAR_SOURCE_STOPPED && reuse_matches(s));
    s->active_reset_epoch=s->sw2->cli.reset_epoch;
    s->owns=1U;s->active_session=s->next_session++;s->begin_us=radar_hw_now_us();
    s->admission_kind=s->sw3->qualification_kind;s->admission_epoch=d->epoch;
    s->source=RADAR_SOURCE_UNKNOWN;s->error=0;s->native_error=0;s->cleanup_error=0;
    s->reported_failure=0U;s->copy_lease=0U;s->config_index=0U;s->shadow_index=0U;s->idle_samples=0U;
    s->copy_bytes=0U;s->copy_fragment=0U;s->copy_fragment_offset=0U;s->copy_crc=UINT32_MAX;
    s->copy_frame_crc=UINT32_MAX;s->observed_packets=0U;
    s->metadata_packets=0U;s->metadata_capable=0U;
    s->copy_wait_polls=0U;
    memset(&s->result,0,sizeof(s->result));memset(&s->snapshot,0,sizeof(s->snapshot));memset(&s->phy,0,sizeof(s->phy));
    s->result.admission_kind=s->admission_kind;
    enter(s,RADAR_SESSION_STOP_BEFORE,s->begin_us);s->guard_us=0U;
    /* Consume only after other gates and SW2 claim succeeded, before any RF
     * command. Teardown resets advance epoch; no history-based auto-renewal. */
    rc=radar_sw3_consume_qualification(s->sw3);
    if (rc) { failure(s,RADAR_SESSION_E_PREREQUISITE,rc);return RADAR_SESSION_E_PREREQUISITE; }
    SAY(s,"[ADMISSION] consumed=%s epoch=%u; full-payload HALT/RESET remain mandatory\r\n",
        radar_sw3_qualification_name(s->admission_kind),(unsigned)s->admission_epoch);
    rc=start_cli(s,RADAR_CAPTURE_CMD_STOP,0U,s->begin_us);
    if (rc) { failure(s,RADAR_SESSION_E_UART,rc);return RADAR_SESSION_E_UART; }
    SAY(s,"[SW4] session=%u profile=%s finite=%u x %uB mode=%s. sensorStop -> %s -> PHY -> SG -> ARM -> %s. RAW transport, not ADC/frame proof.\r\n",
        (unsigned)s->active_session,s->profile->profile_id,(unsigned)s->profile->packet_count,(unsigned)s->profile->packet_bytes,
        s->active_reuse?"REUSE":"FULL",s->active_reuse?"reuse config":"config",s->active_reuse?"sensorStart 0":"sensorStart");
    return 0;
}
void radar_session_cancel(RadarSession *s)
{
    invalidate_reuse(s);
    if (s->exporting) { s->exporting=0U;SAY(s,"[EXPORT CANCELED] Immutable result retained; repeat capture export to restart\r\n"); }
    if (s->owns && s->state!=RADAR_SESSION_FAILED) { failure(s,RADAR_SESSION_E_CANCELED,0); }
}
int radar_session_recover(RadarSession *s)
{
    int rc;
    RadarDma *d=&s->sw3->dma;
    invalidate_reuse(s);
    if (!s->owns||s->state!=RADAR_SESSION_FAILED||s->control->desynced||s->control->pending||
        s->sw2->fatal_control||!s->sw2->wiring_confirmed||d->lease_active||d->state==RADAR_DMA_LOCKED||
        d->state==RADAR_DMA_RESETTING||!d->configured) { return RADAR_SESSION_E_RECOVERY; }
    rc=read_page(s,RADAR_PAGE_FLAGS,&s->flags);
    if (rc || (s->flags&PG)!=PG) { return RADAR_SESSION_E_POWER; }
    /* Remain asserted through reset + RECOVER. Never simply discard a pending
     * UART command and label the old source stopped. */
    rc=radar_sw2_capture_io(s->sw2,0U);
    if (!rc) { rc=radar_sw2_capture_io(s->sw2,5U); }
    if (rc) { return RADAR_SESSION_E_GPIO; }
    s->source=RADAR_SOURCE_UNKNOWN;s->result.valid=0U;
    enter(s,RADAR_SESSION_RECOVERY_HOLD,radar_hw_now_us());
    SAY(s,"[RECOVERY AUTHORIZED] Holding NRST; then one DMA reset, RECOVER only after quiescence. Captured result remains invalid.\r\n");
    return 0;
}
static int copy_step(RadarSession *s)
{
    RadarDma *d=&s->sw3->dma;
    RadarDmaFragment f;
    UINTPTR dst=radar_memory_capture_base();
    u32 count;
    int rc;
    if (!s->copy_lease) {
        rc=radar_dma_acquire(d,&s->copy_packet);
        if (rc==RADAR_DMA_PENDING) { return -1; }
        if (rc) { return rc; }
        s->copy_lease=1U;s->copy_fragment=0U;s->copy_fragment_offset=0U;
        if (s->copy_packet.session!=s->active_session || s->copy_packet.epoch!=s->result.dma_epoch ||
            s->copy_packet.sequence!=d->released_packets+1U) { return -1; }
    }
    rc=radar_dma_fragment(d,&s->copy_packet,s->copy_fragment,&f);if(rc){return rc;}
    if (s->copy_fragment_offset>=f.bytes) { return -1; }
    count=f.bytes-s->copy_fragment_offset;if(count>COPY_QUANTUM){count=COPY_QUANTUM;}
    if (!dst || (u64)s->copy_bytes+count>RADAR_CAPTURE_STORE_BYTES || (u64)s->copy_bytes+count>s->result.bytes) { return -1; }
    memcpy((void *)(dst+s->copy_bytes),(const void *)(f.address+s->copy_fragment_offset),count);
    s->copy_crc=radar_crc32_update(s->copy_crc,(const void *)(dst+s->copy_bytes),count);
    s->copy_frame_crc=radar_crc32_update(s->copy_frame_crc,(const void *)(dst+s->copy_bytes),count);
    s->copy_bytes+=count;s->copy_fragment_offset+=count;
    if (s->copy_fragment_offset==f.bytes) {
        s->copy_fragment_offset=0U;++s->copy_fragment;
        if (s->copy_fragment==s->copy_packet.slot_count) {
            if(s->copy_packet.sequence<=16U)
                s->result.frame_crc32[s->copy_packet.sequence-1U]=s->copy_frame_crc^UINT32_MAX;
            s->copy_frame_crc=UINT32_MAX;
            rc=radar_dma_release(d,&s->copy_packet);if(rc){return rc;}
            s->copy_lease=0U;
        }
    }
    return s->copy_bytes==s->result.bytes?0:1;
}
void radar_session_poll(RadarSession *s)
{
    RadarDma *d=&s->sw3->dma;
    const RadarCaptureProfile *p=s->profile;
    u64 now=radar_hw_now_us();
    int rc;
    check_reuse_epoch(s);
    if (!s->owns) { radar_export_poll(s);return; }
    rc=radar_dma_poll(d,now); /* ONE DMA poller while owned */
    radar_cli_poll(&s->sw2->cli,now); /* ONE UART1 poller while owned */
    if (s->state==RADAR_SESSION_FAILED) {
        if (!s->reported_failure && (d->state==RADAR_DMA_HALTED||d->state==RADAR_DMA_HALT_TIMEOUT||d->state==RADAR_DMA_LOCKED)) {
            s->reported_failure=1U;radar_sw3_print_dma(s->sw3);
        }
        return;
    }
    if (rc<0) { failure(s,RADAR_SESSION_E_DMA,rc);return; }
    while(s->observed_packets<d->completed_packets && s->observed_packets<16U) {
        s->result.frame_observed_us[s->observed_packets++]=now;
    }
    if(s->metadata_capable && s->metadata_packets<d->completed_packets && s->metadata_packets<16U) {
        RadarTransportTime t;int meta=radar_dsp_transport_pop(&t);
        if(meta==1) {
            if(t.sequence!=s->metadata_packets+1U || t.bytes!=p->packet_bytes)t.valid=0U;
            s->result.frame_transport[s->metadata_packets++]=t;
        } else if(meta<0)s->metadata_capable=0U;
    }
    if (++s->phase_polls>=PHASE_POLL_BOUND) { failure(s,RADAR_SESSION_E_TIMEOUT,0);return; }
    if (now-s->guard_us>=1000U) {
        s->guard_us=now;rc=health(s);
        if (rc) { failure(s,rc==RADAR_SESSION_E_POWER?rc:RADAR_SESSION_E_GPIO,rc);return; }
    }
    if (s->state>=RADAR_SESSION_ARM_IDLE && s->state<=RADAR_SESSION_RECEIVE &&
        now-s->armed_us>=p->receive_timeout_us) { failure(s,RADAR_SESSION_E_TIMEOUT,0);return; }
    switch (s->state) {
    case RADAR_SESSION_STOP_BEFORE:
        rc=cli_result(s);if(rc==1){break;}if(rc){failure(s,RADAR_SESSION_E_UART,rc);break;}
        s->source=RADAR_SOURCE_STOPPED;
        rc=recover_pl(s);if(rc){failure(s,RADAR_SESSION_E_RECOVERY,rc);break;}
        enter(s,RADAR_SESSION_RECOVER_BEFORE,now);break;
    case RADAR_SESSION_RECOVER_BEFORE:
        rc=recover_complete(s);
        if(rc==1){if(now-s->phase_us>=CONTROL_WAIT_US){failure(s,RADAR_SESSION_E_TIMEOUT,0);}break;}
        if(rc){failure(s,RADAR_SESSION_E_RECOVERY,rc);break;}
        if (s->active_reuse) {
            if (!reuse_matches(s)) { failure(s,RADAR_SESSION_E_PROFILE,0);break; }
            SAY(s,"[SW4 CFG REUSE] profile=%s reset_epoch=%u; no config commands. sensorStop acknowledged; PHY/SG/ARM and all transport checks retained\r\n",
                p->profile_id,(unsigned)s->reuse_reset_epoch);
            rc=radar_phy_begin(&s->phy,s->control,RADAR_OP_RECAL,now);
            if(rc){failure(s,RADAR_SESSION_E_PHY,rc);break;}
            enter(s,RADAR_SESSION_RECAL,now);break;
        }
        invalidate_reuse(s); /* Full reconfiguration supersedes any old cache. */
        rc=start_cli(s,RADAR_CAPTURE_CMD_CONFIG,0U,now);if(rc){failure(s,RADAR_SESSION_E_UART,rc);break;}
        s->config_index=0U;enter(s,RADAR_SESSION_CONFIG,now);break;
    case RADAR_SESSION_CONFIG:
        if(s->sw2->cli.state==RADAR_CLI_IDLE && !s->sw2->cli.terminal) {
            rc=start_cli(s,RADAR_CAPTURE_CMD_CONFIG,s->config_index,now);
            if(rc){failure(s,RADAR_SESSION_E_UART,rc);}break;
        }
        rc=cli_result(s);if(rc==1){break;}if(rc){failure(s,RADAR_SESSION_E_UART,rc);break;}
        ++s->config_index;
        if (s->config_index<p->command_count) {
            /* Yield one foreground turn with no outstanding UART command.
             * The display may consume a cached frame before the next line. */
            break;
        }
        SAY(s,"[SW4 CFG] %u reviewed lines acknowledged. UART Done does NOT certify LVDS format\r\n",(unsigned)p->command_count);
        rc=radar_phy_begin(&s->phy,s->control,RADAR_OP_RECAL,now);if(rc){failure(s,RADAR_SESSION_E_PHY,rc);break;}
        enter(s,RADAR_SESSION_RECAL,now);break;
    case RADAR_SESSION_RECAL:
        rc=radar_phy_poll(&s->phy,s->control,now);if(rc==1){break;}if(rc){failure(s,RADAR_SESSION_E_PHY,rc);break;}
        enter(s,p->startup_mode==RADAR_CAPTURE_START_IDLE_CLOCK?RADAR_SESSION_RX_READY:RADAR_SESSION_SHADOW,now);break;
    case RADAR_SESSION_RX_READY:
        rc=read_page(s,RADAR_PAGE_PHY_STATUS,&s->phy_status);if(rc){failure(s,RADAR_SESSION_E_GPIO,rc);break;}
        if ((s->phy_status&(RADAR_PHY_RX_READY|RADAR_PHY_BASELINE_VALID))!=(RADAR_PHY_RX_READY|RADAR_PHY_BASELINE_VALID)) {
            if(now-s->phase_us>=500000U){failure(s,RADAR_SESSION_E_PHY,-1);}break;
        }
        if (p->use_delay) {
            u32 baseline,i;
            rc=read_page(s,RADAR_PAGE_BASELINE,&baseline);if(rc){failure(s,RADAR_SESSION_E_PHY,rc);break;}
            for(i=0U;i<3U;++i){if(p->taps[i]<((baseline>>(9U*i))&511U)){rc=-1;break;}}
            if(rc){failure(s,RADAR_SESSION_E_PHY,rc);break;}
            for(i=0U;i<3U;++i){rc=write_shadow(s,(u8)(5U+i),p->taps[i]);if(rc){break;}}
            if(!rc){rc=radar_phy_begin(&s->phy,s->control,RADAR_OP_DELAY_APPLY,now);}
            if(rc){failure(s,RADAR_SESSION_E_PHY,rc);break;}
            enter(s,RADAR_SESSION_DELAY,now);
        } else { enter(s,RADAR_SESSION_SHADOW,now); }
        break;
    case RADAR_SESSION_DELAY:
        rc=radar_phy_poll(&s->phy,s->control,now);if(rc==1){break;}if(rc){failure(s,RADAR_SESSION_E_PHY,rc);break;}
        enter(s,RADAR_SESSION_SHADOW,now);break;
    case RADAR_SESSION_SHADOW: {
        u32 value;
        switch(s->shadow_index) {
        case 0:value=p->packet_bytes/4U;break;case 1:value=p->packet_count;break;
        case 2:value=p->skip_pairs;break;case 3:value=s->active_session;break;
        default:value=UINT32_C(0x16160000)|p->format_flags;break;
        }
        rc=write_shadow(s,(u8)s->shadow_index,value);if(rc){failure(s,RADAR_SESSION_E_GPIO,rc);break;}
        if (++s->shadow_index<5U) { break; }
        rc=radar_dma_prepare(d,p->packet_bytes,p->packet_count,s->active_session,p->receive_timeout_us);
        if(!rc){rc=radar_dma_start(d);}if(rc){failure(s,RADAR_SESSION_E_DMA,rc);break;}
        s->result.dma_epoch=d->epoch;enter(s,RADAR_SESSION_DMA_RUNNING,now);break;
    }
    case RADAR_SESSION_DMA_RUNNING: {
        RadarDmaRegisters regs;
        rc=radar_dma_probe(&regs);if(rc){failure(s,RADAR_SESSION_E_DMA,rc);break;}
        if((regs.status&XAXIDMA_HALTED_MASK)||!(regs.control&XAXIDMA_CR_RUNSTOP_MASK)){break;}
        rc=read_page(s,RADAR_PAGE_PHY_STATUS,&s->phy_status);if(rc){failure(s,RADAR_SESSION_E_GPIO,rc);break;}
        if(!(s->phy_status&RADAR_PHY_ARM_ADMISSIBLE)){failure(s,RADAR_SESSION_E_PHY,-1);break;}
        /* Optional new hardware observer. Old BSP never reads this address.
         * Missing/raced metadata changes time trust, not transport validity. */
        s->metadata_capable=(u8)(radar_dsp_transport_begin()==0);
        s->armed_us=now;rc=exec(s,RADAR_OP_ARM,0U);if(rc){failure(s,RADAR_SESSION_E_GPIO,rc);break;}
        s->idle_samples=0U;s->qualify_us=now;
        if(p->startup_mode==RADAR_CAPTURE_START_IDLE_CLOCK){enter(s,RADAR_SESSION_ARM_IDLE,now);}
        else {
            s->source=RADAR_SOURCE_STARTING;
            rc=start_cli(s,s->active_reuse?RADAR_CAPTURE_CMD_START_REUSE:RADAR_CAPTURE_CMD_START,0U,now);if(rc){failure(s,RADAR_SESSION_E_UART,rc);break;}
            enter(s,RADAR_SESSION_START,now);
        }
        break;
    }
    case RADAR_SESSION_ARM_IDLE: {
        u32 wanted=RADAR_FLAG_SESSION_OPEN|RADAR_FLAG_RX_READY|RADAR_FLAG_DESER_RESETN|
                   RADAR_FLAG_FIFO_RESETN|RADAR_FLAG_RX_IDLE;
        if (now-s->qualify_us<100U) { break; }s->qualify_us=now;
        rc=read_page(s,RADAR_PAGE_FLAGS,&s->flags);if(rc){failure(s,RADAR_SESSION_E_GPIO,rc);break;}
        if(s->flags&(RADAR_FLAG_FIRST|RADAR_FLAG_SOURCE_COMPLETE)){failure(s,RADAR_SESSION_E_DATA,-1);break;}
        if((s->flags&wanted)==wanted && !(s->flags&(RADAR_FLAG_ARM_PENDING|RADAR_FLAG_CMD_PENDING))) { ++s->idle_samples; }
        else { s->idle_samples=0U; }
        if(s->idle_samples>=2U) {
            s->source=RADAR_SOURCE_STARTING;
            rc=start_cli(s,s->active_reuse?RADAR_CAPTURE_CMD_START_REUSE:RADAR_CAPTURE_CMD_START,0U,now);if(rc){failure(s,RADAR_SESSION_E_UART,rc);break;}
            enter(s,RADAR_SESSION_START,now);
        }
        break;
    }
    case RADAR_SESSION_START:
        rc=cli_result(s);if(rc==1){break;}if(rc){failure(s,RADAR_SESSION_E_UART,rc);break;}
        s->source=RADAR_SOURCE_RUNNING;enter(s,RADAR_SESSION_RECEIVE,now);break;
    case RADAR_SESSION_RECEIVE:
        if(d->completed_packets!=p->packet_count){break;}
        rc=read_page(s,RADAR_PAGE_FLAGS,&s->flags);if(rc){failure(s,RADAR_SESSION_E_GPIO,rc);break;}
        if(!(s->flags&RADAR_FLAG_SOURCE_COMPLETE)){break;}
        if(d->ring->HwCnt||d->partial_prefix||d->completed_bytes!=(u64)p->packet_bytes*p->packet_count){failure(s,RADAR_SESSION_E_DATA,-1);break;}
        radar_snapshot_begin(&s->snapshot,now,p->snapshot_timeout_us);enter(s,RADAR_SESSION_SNAPSHOT,now);break;
    case RADAR_SESSION_SNAPSHOT:
        rc=radar_snapshot_poll(&s->snapshot,s->control,now);if(rc==1){break;}if(rc){failure(s,RADAR_SESSION_E_DATA,rc);break;}
        rc=radar_snapshot_matches(&s->snapshot.data,p,s->active_session);if(rc){failure(s,RADAR_SESSION_E_DATA,rc);break;}
        s->result.snapshot=s->snapshot.data;
        rc=start_cli(s,RADAR_CAPTURE_CMD_STOP,0U,now);if(rc){failure(s,RADAR_SESSION_E_UART,rc);break;}
        enter(s,RADAR_SESSION_STOP_AFTER,now);break;
    case RADAR_SESSION_STOP_AFTER:
        rc=cli_result(s);if(rc==1){break;}if(rc){failure(s,RADAR_SESSION_E_UART,rc);break;}
        s->source=RADAR_SOURCE_STOPPED;
        rc=health(s);if(rc||s->sys_faults){failure(s,RADAR_SESSION_E_DATA,rc);break;}
        rc=radar_dma_stop_begin(d);if(rc<0){failure(s,RADAR_SESSION_E_DMA,rc);break;}
        enter(s,RADAR_SESSION_HALT,now);break;
    case RADAR_SESSION_HALT:
        if(d->state!=RADAR_DMA_HALTED){break;}
        if(d->halt_result!=RADAR_DMA_PHASE_PASS||!radar_dma_is_quiescent(d)){failure(s,RADAR_SESSION_E_DMA,RADAR_DMA_E_DEADLINE);break;}
        /* Unlike the no-input diagnostic, this HALT follows ALL real BDs.
         * Reset fallback can recover memory but can NEVER validate this run. */
        if (d->stop_was_incomplete || d->ring->HwCnt || d->partial_prefix ||
            d->completed_packets!=p->packet_count || d->completed_bds!=d->submitted_bds ||
            d->completed_bytes!=(u64)p->packet_bytes*p->packet_count ||
            d->packet_queued!=p->packet_count || d->lease_active) {
            failure(s,RADAR_SESSION_E_DATA,RADAR_DMA_E_BOUNDARY);break;
        }
        ++s->normal_halt_passes;
        s->result.payload_halt_result=d->halt_result;s->result.payload_halt_us=d->halt_elapsed_us;
        s->result.session=s->active_session;s->result.bytes=p->packet_bytes*p->packet_count;
        s->result.packet_bytes=p->packet_bytes;s->result.packet_count=p->packet_count;s->result.format=p->format_flags;
        s->result.skip_pairs=p->skip_pairs;s->result.startup_mode=p->startup_mode;
        s->result.begin_us=s->begin_us;s->result.armed_us=s->armed_us;s->result.complete_us=now;
        (void)radar_dma_registers(d,&s->result.final_dma);
        enter(s,RADAR_SESSION_COPY,now);break;
    case RADAR_SESSION_COPY:
        if(s->raw_store_owner) {
            ++s->copy_wait_polls;
            if(now-s->phase_us>=UINT64_C(30000000))failure(s,RADAR_SESSION_E_OWNER,0);
            break; /* RF stopped, DMA HALTED: preserve RX and old CPU store. */
        }
        if(!s->copy_bytes)s->result.copy_begin_us=radar_hw_now_us();
        rc=copy_step(s);if(rc==1){break;}if(rc){failure(s,RADAR_SESSION_E_MEMORY,rc);break;}
        if(s->copy_lease||d->packet_queued||d->ring->FreeCnt!=(int)RADAR_RX_BD_COUNT){failure(s,RADAR_SESSION_E_OWNER,0);break;}
        s->result.crc32=s->copy_crc^UINT32_MAX;
        /* CPU store survives teardown. Flush supports subsequent debugger read,
         * not DMA access. Source/BD memory is never aliased with this region. */
        Xil_DCacheFlushRange(radar_memory_capture_base(),s->result.bytes);radar_hw_barrier();
        s->result.copy_complete_us=radar_hw_now_us();
        rc=radar_dma_reset_begin(d);if(rc<0){failure(s,RADAR_SESSION_E_DMA,rc);break;}
        enter(s,RADAR_SESSION_RESET,now);break;
    case RADAR_SESSION_RESET:
        if(d->state!=RADAR_DMA_READY){break;}
        if(d->reset_result!=RADAR_DMA_PHASE_PASS || !d->reset_confirmed ||
            !radar_dma_is_quiescent(d) || d->packet_queued || d->lease_active ||
            d->ring->FreeCnt!=(int)RADAR_RX_BD_COUNT || d->ring->HwCnt ||
            d->ring->PreCnt || d->ring->PostCnt || d->epoch!=s->admission_epoch+1U) {
            failure(s,RADAR_SESSION_E_DMA,RADAR_DMA_E_DEADLINE);break;
        }
        s->result.teardown_reset_result=d->reset_result;s->result.teardown_reset_us=d->reset_elapsed_us;
        rc=radar_sw2_capture_io(s->sw2,0U);if(rc){failure(s,RADAR_SESSION_E_GPIO,rc);break;}
        rc=recover_pl(s);if(rc){failure(s,RADAR_SESSION_E_RECOVERY,rc);break;}
        enter(s,RADAR_SESSION_RECOVER_AFTER,now);break;
    case RADAR_SESSION_RECOVER_AFTER:
        rc=recover_complete(s);if(rc==1){if(now-s->phase_us>=CONTROL_WAIT_US){failure(s,RADAR_SESSION_E_TIMEOUT,0);}break;}
        if(rc){failure(s,RADAR_SESSION_E_RECOVERY,rc);break;}
        rc=radar_sw2_capture_release(s->sw2,0);
        if(rc){failure(s,RADAR_SESSION_E_OWNER,rc);break;}
        s->result.ready_us=radar_hw_now_us();
        s->result.valid=1U;++s->captures_passed;s->owns=0U;
        if(s->active_reuse) { ++s->reuse_completed; } else { ++s->full_config_completed; }
        remember_completed_config(s);
        enter(s,RADAR_SESSION_DONE,now);
        SAY(s,"RESULT: PASS SW4 FINITE_RAW16_TRANSPORT session=%u bytes=%u crc32=%08x. Source stop ACK, real BD/length/EOF, fresh snapshot, halt/reset/recover complete.\r\n",
            (unsigned)s->result.session,(unsigned)s->result.bytes,(unsigned)s->result.crc32);
        SAY(s,"[RESULT] CPU copy=%08x; capture export outputs CRC-checked log frames. ADC/IQ/chirp/frame semantics NOT validated. Before next run: capture clear, dma qualify, connect/query, capture preflight.\r\n",(unsigned)radar_memory_capture_base());
        if(s->profile->qualification==RADAR_CAPTURE_FIRST_PACKET_TEST){
            SAY(s,"[FIRST_PACKET_TEST] Transport experiment only; input timing and actual wire format remain pending. No ADC/frame signoff.\r\n");
        }else if(s->profile->qualification==RADAR_CAPTURE_FINITE_FRAME_TEST){
            SAY(s,"[FINITE_FRAME_TEST] Finite source transport retained; strict offline HSI/IQ parsing is still required. Input timing remains pending.\r\n");
        }
        break;
    case RADAR_SESSION_RECOVERY_HOLD:
        if(now-s->phase_us<RADAR_RESET_HOLD_US){break;}
        rc=health(s);if(rc||s->io_request!=5U||!(s->flags&RADAR_FLAG_NRST_LOW)){failure(s,RADAR_SESSION_E_RECOVERY,rc);break;}
        s->source=RADAR_SOURCE_HELD_RESET;
        rc=radar_dma_reset_authorized(d);if(rc<0){failure(s,RADAR_SESSION_E_RECOVERY,rc);break;}
        enter(s,RADAR_SESSION_RECOVERY_DMA,now);break;
    case RADAR_SESSION_RECOVERY_DMA:
        if(d->state!=RADAR_DMA_READY){break;}
        if(d->reset_result!=RADAR_DMA_PHASE_PASS){failure(s,RADAR_SESSION_E_RECOVERY,RADAR_DMA_E_DEADLINE);break;}
        rc=recover_pl(s);if(rc){failure(s,RADAR_SESSION_E_RECOVERY,rc);break;}
        enter(s,RADAR_SESSION_RECOVERY_PL,now);break;
    case RADAR_SESSION_RECOVERY_PL:
        rc=recover_complete(s);if(rc==1){if(now-s->phase_us>=CONTROL_WAIT_US){failure(s,RADAR_SESSION_E_TIMEOUT,0);}break;}
        if(rc){failure(s,RADAR_SESSION_E_RECOVERY,rc);break;}
        rc=radar_sw2_capture_io(s->sw2,0U);if(rc){failure(s,RADAR_SESSION_E_GPIO,rc);break;}
        s->source=RADAR_SOURCE_UNKNOWN;enter(s,RADAR_SESSION_RECOVERY_BOOT,now);break;
    case RADAR_SESSION_RECOVERY_BOOT:
        if(now-s->phase_us<RADAR_RESET_BOOT_US){break;}
        rc=health(s);if(rc||!(s->flags&RADAR_FLAG_NERR_N)){failure(s,RADAR_SESSION_E_POWER,rc);break;}
        radar_cli_new_reset_epoch(&s->sw2->cli);
        s->source=RADAR_SOURCE_STOPPED; /* confirmed non-autostart profile + physical reset sequence */
        rc=radar_sw2_capture_release(s->sw2,0);
        if(rc){failure(s,RADAR_SESSION_E_OWNER,rc);break;}
        s->owns=0U;++s->recoveries;s->confirmed=0U;
        enter(s,RADAR_SESSION_IDLE,now);
        SAY(s,"[RECOVERED] NRST hold/release + boot wait, DMA reset and PL RECOVER completed. Previous capture stays FAIL. Re-run dma qualify (or qualify10), reconnect/query and capture confirm before another run.\r\n");
        break;
    default:failure(s,RADAR_SESSION_E_OWNER,0);break;
    }
}
const char *radar_session_state_name(RadarSessionState st)
{
    static const char *const names[]={"IDLE","SENSOR_STOP_BEFORE","RECOVER_BEFORE","CONFIG",
        "RECAL","RX_READY","DELAY","SHADOW","DMA_RUNNING","ARM_IDLE","SENSOR_START",
        "RECEIVE","SNAPSHOT","SENSOR_STOP_AFTER","HALT","COPY","RESET","RECOVER_AFTER",
        "DONE","FAILED_QUARANTINED","RECOVERY_HOLD","RECOVERY_DMA","RECOVERY_PL","RECOVERY_BOOT"};
    return (u32)st<sizeof(names)/sizeof(names[0])?names[st]:"UNKNOWN";
}
const char *radar_session_error_name(int error)
{
    switch(error) {
    case 0:return "OK";case RADAR_SESSION_E_PROFILE:return "PROFILE_OR_CONFIRMATION_REQUIRED";
    case RADAR_SESSION_E_PREREQUISITE:return "SW3_SW2_OR_VERSION_PREREQUISITE";
    case RADAR_SESSION_E_OWNER:return "OWNERSHIP_OR_RETAINED_RESULT";
    case RADAR_SESSION_E_GPIO:return "GPIO_OR_HEALTH";case RADAR_SESSION_E_UART:return "UART_TRANSACTION";
    case RADAR_SESSION_E_PHY:return "PHY_QUALIFICATION_OR_EXECUTION";case RADAR_SESSION_E_TIMEOUT:return "PHASE_DEADLINE";
    case RADAR_SESSION_E_DATA:return "TRANSPORT_OR_SNAPSHOT_MISMATCH";case RADAR_SESSION_E_CANCELED:return "USER_CANCELED";
    case RADAR_SESSION_E_DMA:return "DMA_LIFECYCLE";case RADAR_SESSION_E_POWER:return "PG_NERR_OR_IO_POWER";
    case RADAR_SESSION_E_RECOVERY:return "RECOVERY_REFUSED_OR_FAILED";case RADAR_SESSION_E_MEMORY:return "CPU_COPY_OR_OWNERSHIP";
    default:return "UNKNOWN";
    }
}
void radar_session_print_status(RadarSession *s)
{
    print_reuse(s);
    SAY(s,"[SW4] state=%s owns=%u source=%u confirmed=%u profile=%s qualified=%u session=%u\r\n",
        radar_session_state_name(s->state),(unsigned)s->owns,(unsigned)s->source,(unsigned)s->confirmed,
        s->profile->profile_id,(unsigned)(radar_capture_profile_validate(s->profile)==0),(unsigned)s->active_session);
    SAY(s,"[SW4] captures_passed=%u failed=%u recoveries=%u error=%s native=%d failed_phase=%s cleanup=%d\r\n",
        (unsigned)s->captures_passed,(unsigned)s->captures_failed,(unsigned)s->recoveries,
        radar_session_error_name(s->error),s->native_error,radar_session_state_name(s->failed_phase),s->cleanup_error);
    SAY(s,"[SW4 last sample] flags=%08x SYS_fault=%08x PHY=%08x PHY_fault=%08x io=%08x\r\n",
        (unsigned)s->flags,(unsigned)s->sys_faults,(unsigned)s->phy_status,(unsigned)s->phy_fault,(unsigned)s->io_request);
    SAY(s,"[SESSION EVIDENCE] entry=%s epoch=%u normal_halt_passes=%u; result_halt=%s/%uus result_reset=%s/%uus\r\n",
        radar_sw3_qualification_name(s->admission_kind),(unsigned)s->admission_epoch,
        (unsigned)s->normal_halt_passes,radar_dma_phase_name(s->result.payload_halt_result),
        (unsigned)s->result.payload_halt_us,radar_dma_phase_name(s->result.teardown_reset_result),
        (unsigned)s->result.teardown_reset_us);
    SAY(s,"[RESULT] valid=%u copied=%u bytes=%u crc=%08x export=%u offset=%u; ADC semantics NOT_RUN\r\n",
        (unsigned)s->result.valid,(unsigned)s->copy_bytes,(unsigned)s->result.bytes,(unsigned)s->result.crc32,
        (unsigned)s->exporting,(unsigned)s->export_offset);
}
static void print_profile(RadarSession *s)
{
    const RadarCaptureProfile *p=s->profile;
    SAY(s,"[CAPTURE PROFILE] name=%s id=%s validation=%d approvals=%02x\r\n",p->name,p->profile_id,radar_capture_profile_validate(p),(unsigned)p->approvals);
    SAY(s,"[QUALIFICATION] %s source_reviews=%02x; source review is not a live timing measurement\r\n",p->qualification==RADAR_CAPTURE_FIRST_PACKET_TEST?"FIRST_PACKET_TEST":p->qualification==RADAR_CAPTURE_FINITE_FRAME_TEST?"FINITE_FRAME_TEST":p->qualification==RADAR_CAPTURE_STREAM_FRAME_TEST?"STREAM_FRAME_TEST":"QUALIFIED",(unsigned)p->source_review_flags);
    SAY(s,"[EXPECTED BIN] %s\r\n[EXPECTED CFG] %s\r\n[REVIEW] %s\r\n",p->firmware_sha256,p->cfg_sha256,p->review_sha256);
    SAY(s,"[CONTRACT] lanes=%u rate=%uMb/s word=%u format=%u bytes=%u count=%u skip_pairs=%u startup=%u commands=%u\r\n",
        (unsigned)p->lanes,(unsigned)p->lane_mbps,(unsigned)p->word_bits,(unsigned)p->format_flags,
        (unsigned)p->packet_bytes,(unsigned)p->packet_count,(unsigned)p->skip_pairs,(unsigned)p->startup_mode,(unsigned)p->command_count);
    SAY(s,"[BOUNDARY] Expected hashes and USER attestation, not live flash/voltage/timing measurement. generate profile from actual files; never force approval bits.\r\n");
}
int radar_session_command(RadarSession *s,const char *cmd)
{
    int rc;
    check_reuse_epoch(s);
    if (strcmp(cmd,"capture reuse status")==0) { print_reuse(s);return 1; }
    if (strcmp(cmd,"capture preflight")==0) { radar_session_print_preflight(s);return 1; }
    if (strcmp(cmd,"capture status")==0) { radar_session_print_status(s);return 1; }
    if (strcmp(cmd,"capture profile")==0||strcmp(cmd,"capture plan")==0) { print_profile(s);return 1; }
    if (strcmp(cmd,"progress")==0) { radar_session_print_status(s);return 0; }
    if (strcmp(cmd,"help")==0) {
        SAY(s,"SW4.2: capture preflight | capture profile/plan/status | capture batch 1/8/16 | capture confirm ID | capture once | capture export | capture clear | capture recover\r\n");
        SAY(s,"capture reuse on|off|status: exact completed config only, default OFF, no retry on error\r\n");
        if (s->owns) { return 1; }return 0;
    }
    if (strcmp(cmd,"safe")==0||strcmp(cmd,"disable")==0) {
        invalidate_reuse(s);
        if(s->owns||s->exporting){radar_session_cancel(s);return 1;}return 0;
    }
    if (strcmp(cmd,"capture recover")==0) {
        rc=radar_session_recover(s);if(rc){SAY(s,"[REFUSE] %s; no unknown-state reset retry\r\n",radar_session_error_name(rc));}return 1;
    }
    if (s->owns) {
        if(strcmp(cmd,"dma status")==0||strcmp(cmd,"dma trace")==0){return 0;}
        SAY(s,"[BUSY] SW4 exclusively owns UART/GPIO/DMA; capture status/profile/recover, dma status/trace, progress, safe only\r\n");return 1;
    }
    if (strcmp(cmd,"capture reuse on")==0 || strcmp(cmd,"capture reuse off")==0) {
        if (s->exporting || s->raw_store_owner ||
            (s->state!=RADAR_SESSION_IDLE && s->state!=RADAR_SESSION_DONE) ||
            s->sw3->job!=SW3_IDLE || s->sw2->job!=SW2_JOB_NONE ||
            s->sw3->dma.lease_active) {
            SAY(s,"[REFUSE] capture reuse requires idle capture/processing with no owner or export\r\n");return 1;
        }
        s->reuse_enabled=(u8)(strcmp(cmd,"capture reuse on")==0);
        invalidate_reuse(s); /* Explicit enable always starts with full config. */
        print_reuse(s);return 1;
    }
    if (strncmp(cmd,"capture confirm ",16U)==0) {
        if(radar_capture_profile_validate(s->profile)||strcmp(cmd+16U,s->profile->profile_id)!=0||!s->sw2->wiring_confirmed){
            SAY(s,"[REFUSE] Requires qualified generated profile, confirm io, exact profile ID\r\n");return 1;
        }
        s->confirmed=1U;
        if(s->profile->qualification==RADAR_CAPTURE_FIRST_PACKET_TEST){
            SAY(s,"[TEST CONFIRMED] Profile %s: one 128 KiB experiment with electrical/image/geometry and source compatibility reviews. Input timing and actual wire format remain pending; capture once starts RF.\r\n",s->profile->profile_id);
        }else if(s->profile->qualification==RADAR_CAPTURE_FINITE_FRAME_TEST){
            SAY(s,"[TEST CONFIRMED] Profile %s: finite-frame transport experiment. ADC/frame parsing and input timing remain pending; capture once starts RF.\r\n",s->profile->profile_id);
        }else if(s->profile->qualification==RADAR_CAPTURE_STREAM_FRAME_TEST){
            SAY(s,"[STREAM CONFIRMED] Profile %s: reviewed streaming transport identity. Physical input timing remains pending; only explicit scope live starts RF.\r\n",s->profile->profile_id);
        }else{
            SAY(s,"[USER ATTESTED] Actual flashed bin/cfg and electrical/source timing match profile %s; explicit RF start is allowed only by capture once\r\n",s->profile->profile_id);
        }
        return 1;
    }
    if(strncmp(cmd,"capture batch ",14U)==0) {
        const RadarCaptureProfile *next=NULL;
        if(!strcmp(cmd+14U,"1"))next=radar_capture_profile_batch(1U);
        else if(!strcmp(cmd+14U,"8"))next=radar_capture_profile_batch(8U);
        else if(!strcmp(cmd+14U,"16"))next=radar_capture_profile_batch(16U);
        if(!next || s->exporting || s->result.valid || s->raw_store_owner || s->state!=RADAR_SESSION_IDLE ||
           s->sw3->job!=SW3_IDLE || s->sw2->job!=SW2_JOB_NONE || s->sw3->dma.lease_active) {
            SAY(s,"[REFUSE] capture batch 1|8|16 requires empty idle finite capture; export/clear retained data first.\r\n");return 1;
        }
        invalidate_reuse(s);s->profile=next;s->confirmed=0U;
        SAY(s,"[CAPTURE BATCH] frames=%u profile=%s. Offline-reviewed contract; confirm this exact profile before RF. No source start.\r\n",
            (unsigned)next->packet_count,next->profile_id);return 1;
    }
    if (strcmp(cmd,"capture once")==0) {
        rc=radar_session_start(s);if(rc){SAY(s,"[REFUSE] capture once: %s. Run capture preflight: need fresh epoch-bound dma qualify, SW2 check10/reset3, current version, generated profile, confirm, empty result\r\n",radar_session_error_name(rc));}return 1;
    }
    if (strcmp(cmd,"capture export")==0) {
        if(!s->result.valid||s->state!=RADAR_SESSION_DONE||s->exporting){SAY(s,"[REFUSE] Export needs one validated, retained SW4 result\r\n");return 1;}
        s->export_index=0U;s->export_offset=0U;s->exporting=1U;return 1;
    }
    if (strcmp(cmd,"capture clear")==0) {
        if(s->exporting){SAY(s,"[REFUSE] Finish/cancel export before clear\r\n");return 1;}
        memset(&s->result,0,sizeof(s->result));s->copy_bytes=0U;enter(s,RADAR_SESSION_IDLE,radar_hw_now_us());
        SAY(s,"[RESULT CLEARED] CPU result invalidated, not erased. No hardware operation. Next: dma qualify, connect/query, capture preflight\r\n");return 1;
    }
    if(strncmp(cmd,"capture",7U)==0&&(cmd[7]=='\0'||cmd[7]==' ')){SAY(s,"[REJECT] Unknown capture command; type help\r\n");return 1;}
    return 0;
}
