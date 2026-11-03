/* SPDX-License-Identifier: MIT
 * SW3.2 independent reset qualification and no-input halt diagnostics. No synthetic stream, no ARM.
 * HALT_TIMEOUT never automatically falls through to RESET: `dma reset` is
 * a separate, user-authorized, no-producer recovery. A reset does not fix the
 * recorded halt test result. A final RESET timeout requires target reset.
 */
#include "radar_sw3.h"
#include "radar_hw.h"
#include "xil_cache.h"
#include <string.h>
#define SAY(s,...) ((void)radar_console_printf((s)->console,__VA_ARGS__))
#define CHECK_BYTES 131072U
#define CHECK_TIME_US 20000U
#define CHECK_LIMIT_US 1000000U
#define CHECK_SESSION UINT32_C(0x53573300)

const char *radar_sw3_qualification_name(RadarQualificationKind kind)
{
    switch (kind) {
    case RADAR_QUAL_RESET_CONTROL: return "RUN_RESET";
    case RADAR_QUAL_RUN_HALT_RESET: return "RUN_HALT_RESET";
    default: return "NONE";
    }
}
const char *radar_sw3_qualification_reason(RadarQualificationReason reason)
{
    static const char *const names[] = {
        "NOT_TESTED", "VALID", "NEW_TEST", "TEST_FAILED", "EXPLICIT_RESET",
        "STALE_EPOCH", "DMA_NOT_READY", "CAPTURE_CONSUMED", "SESSION_FAILED",
        "CANCELED"
    };
    return (u32)reason < sizeof(names)/sizeof(names[0]) ? names[reason] : "UNKNOWN";
}
void radar_sw3_revoke_qualification(RadarSw3 *s, RadarQualificationReason reason)
{
    if (!s) { return; }
    s->reset_qualified=0U;
    s->control_qualified=0U;
    s->qualification_reason=reason;
    /* Retain issued kind/epoch for diagnostics; flags above are the permit. */
}
static int qualification_memory_ready(const RadarSw3 *s)
{
    const RadarDma *d=&s->dma;
    u32 i;
    if (s->job!=SW3_IDLE || d->state!=RADAR_DMA_READY || !d->configured ||
        !d->ring_created || !d->ring || !d->reset_confirmed || d->lease_active ||
        d->packet_queued || d->partial_prefix || d->last_error ||
        d->ring->FreeCnt!=(int)RADAR_RX_BD_COUNT || d->ring->PreCnt ||
        d->ring->HwCnt || d->ring->PostCnt) { return 0; }
    for (i=0U;i<RADAR_RX_BD_COUNT;++i) {
        if (d->slot[i].owner!=RADAR_BUFFER_FREE) { return 0; }
    }
    return 1;
}
static void expire_qualification(RadarSw3 *s)
{
    if (!s->reset_qualified) { return; }
    if (s->qualification_epoch!=s->dma.epoch ||
        s->qualification_reset_count!=s->dma.reset_count) {
        radar_sw3_revoke_qualification(s,RADAR_QUAL_STALE_EPOCH);
    } else if (!qualification_memory_ready(s)) {
        radar_sw3_revoke_qualification(s,RADAR_QUAL_DMA_NOT_READY);
    }
}
int radar_sw3_capture_ready(RadarSw3 *s)
{
    if (!s) { return 0; }
    expire_qualification(s);
    if (!s->reset_qualified || s->qualification_reason!=RADAR_QUAL_VALID) { return 0; }
    if (!radar_dma_is_quiescent(&s->dma) || radar_dma_ring_check(&s->dma)!=0) {
        radar_sw3_revoke_qualification(s,RADAR_QUAL_DMA_NOT_READY);
        return 0;
    }
    return 1;
}
int radar_sw3_consume_qualification(RadarSw3 *s)
{
    if (!radar_sw3_capture_ready(s)) { return RADAR_DMA_E_OWNER; }
    radar_sw3_revoke_qualification(s,RADAR_QUAL_CONSUMED);
    return 0;
}
void radar_sw3_print_qualification(RadarSw3 *s)
{
    int ready=radar_sw3_capture_ready(s);
    SAY(s,"[QUAL] capture_entry=%u kind=%s reason=%s issued_epoch=%u current_epoch=%u reset_serial=%u/%u one_use=1\r\n",
        (unsigned)ready,radar_sw3_qualification_name(s->qualification_kind),
        radar_sw3_qualification_reason(s->qualification_reason),
        (unsigned)s->qualification_epoch,(unsigned)s->dma.epoch,
        (unsigned)s->qualification_reset_count,(unsigned)s->dma.reset_count);
}
static int quiet_guard(RadarSw3 *s)
{
    int rc;
    if (s->sw2->state!=SW2_SAFE || s->sw2->fatal_control ||
        s->control->desynced || !s->control->abi_verified ||
        radar_cli_busy(&s->sw2->cli) || s->sw2->cli.enabled) { return RADAR_DMA_E_OWNER; }
    rc=radar_control_read32(s->control,RADAR_PAGE_FLAGS,&s->guard_flags);
    if (rc==0) { rc=radar_control_read32(s->control,RADAR_PAGE_IO_REQUEST,&s->guard_io); }
    if (rc || s->guard_io ||
        (s->guard_flags & (RADAR_SW1_ACTIVITY_MASK|RADAR_SW1_OUTPUT_MASK))) { return RADAR_DMA_E_OWNER; }
    return 0;
}
static void print_bd(RadarSw3 *s,const RadarDmaBdView *b)
{
    if (!b->valid) { return; }
    SAY(s,"[BD] slot=%u owner=%u addr=%08x next=%08x:%08x buf=%08x:%08x ctrl=%08x actual_status=%08x\r\n",
        (unsigned)b->slot,(unsigned)b->owner,(unsigned)b->address,
        (unsigned)b->next_hi,(unsigned)b->next_lo,(unsigned)b->buffer_hi,(unsigned)b->buffer_lo,
        (unsigned)b->control,(unsigned)b->status);
}
void radar_sw3_print_dma(RadarSw3 *s)
{
    RadarDmaRegisters r;
    RadarDma *d=&s->dma;
    int rc;
    (void)radar_sw3_capture_ready(s);
    rc=radar_dma_registers(d,&r);
    unsigned i;
    SAY(s,"[DMA] state=%s probe=%s epoch=%u reset_confirmed=%u lease=%u full_control_qualified=%u\r\n",
        radar_dma_state_name(d->state),radar_dma_error_name(rc),(unsigned)d->epoch,
        (unsigned)d->reset_confirmed,(unsigned)d->lease_active,(unsigned)s->control_qualified);
    if (rc) { return; }
    SAY(s,"[DMA REG] CR=%08x SR=%08x CUR=%08x TAIL=%08x free/pre/hw/post=%u/%u/%u/%u\r\n",
        (unsigned)r.control,(unsigned)r.status,(unsigned)r.current,(unsigned)r.tail,
        (unsigned)r.free_count,(unsigned)r.pre_count,(unsigned)r.hw_count,(unsigned)r.post_count);
    SAY(s,"[DMA PHASE] halt=%s (%u us) reset=%s (%u us) reset_requests=%u completed_resets=%u max_poll_gap=%u us\r\n",
        radar_dma_phase_name(d->halt_result),(unsigned)d->halt_elapsed_us,
        radar_dma_phase_name(d->reset_result),(unsigned)d->reset_elapsed_us,
        (unsigned)d->reset_requests,(unsigned)d->reset_count,(unsigned)d->max_poll_gap_us);
    SAY(s,"[DMA PAYLOAD] packets=%u BDs=%u bytes=%u:%08x partial_BD=%u queued=%u last_error=%s\r\n",
        (unsigned)d->completed_packets,(unsigned)d->completed_bds,(unsigned)(d->completed_bytes>>32U),
        (unsigned)d->completed_bytes,(unsigned)d->partial_prefix,(unsigned)d->packet_queued,
        radar_dma_error_name(d->last_error));
    for (i=0U;i<3U;++i) {
        /* CUR often equals BD1: do not print it a second time. */
        if (i==2U && r.bd[i].valid &&
            (r.bd[i].address==r.bd[0].address || r.bd[i].address==r.bd[1].address)) { continue; }
        print_bd(s,&r.bd[i]);
    }
    radar_sw3_print_qualification(s);
}
static void print_trace(RadarSw3 *s)
{
    RadarDma *d=&s->dma;
    u32 i,n=d->trace_count<6U?d->trace_count:6U;
    if (radar_console_free(s->console)<6144U) { SAY(s,"[BUSY] Drain console before dma trace\r\n");return; }
    SAY(s,"[TRACE] last %u/%u events; event 3/4/5=halt request/done/timeout; 6/7/8=reset request/done/timeout\r\n",(unsigned)n,(unsigned)d->trace_count);
    for (i=0;i<n;++i) {
        const RadarDmaTrace *t=&d->trace[(d->trace_head+RADAR_DMA_TRACE_DEPTH-n+i)%RADAR_DMA_TRACE_DEPTH];
        SAY(s,"[TRACE] t=%u:%08x event=%u state=%s written=%08x CR=%08x SR=%08x CUR=%08x TAIL=%08x elapsed=%u err=%d\r\n",
            (unsigned)(t->registers.sampled_us>>32U),(unsigned)t->registers.sampled_us,
            (unsigned)t->event,radar_dma_state_name((RadarDmaState)t->state),(unsigned)t->written_cr,
            (unsigned)t->registers.control,(unsigned)t->registers.status,
            (unsigned)t->registers.current,(unsigned)t->registers.tail,(unsigned)t->elapsed_us,t->error);
        print_bd(s,&t->registers.bd[0]); print_bd(s,&t->registers.bd[1]);
    }
}
static u32 canary_word(u32 slot,u32 line,u32 word)
{
    return UINT32_C(0xA5693C17)^(slot<<20U)^(line<<12U)^(word*UINT32_C(0x01020304));
}
static void write_canaries(RadarSw3 *s)
{
    u32 slot,line,k;
    for (slot=0;slot<2U;++slot) for (line=0;line<2U;++line) {
        volatile u32 *p=(volatile u32 *)(s->dma.rx_base+(UINTPTR)slot*RADAR_RX_SLICE_BYTES+
                       (line?RADAR_RX_SLICE_BYTES-RADAR_CACHE_LINE_BYTES:0U));
        for (k=0;k<16U;++k) { p[k]=canary_word(slot,line,k); }
    }
    radar_hw_barrier();
}
static int verify_canaries(RadarSw3 *s)
{
    u32 slot,line,k;
    if (s->dma.state!=RADAR_DMA_READY || !s->dma.reset_confirmed || !radar_dma_is_quiescent(&s->dma)) { return RADAR_DMA_E_STATE; }
    for (slot=0;slot<2U;++slot) {
        Xil_DCacheInvalidateRange(s->dma.rx_base+(UINTPTR)slot*RADAR_RX_SLICE_BYTES,RADAR_RX_SLICE_BYTES);
        radar_hw_barrier();
        for (line=0;line<2U;++line) {
            volatile u32 *p=(volatile u32 *)(s->dma.rx_base+(UINTPTR)slot*RADAR_RX_SLICE_BYTES+
                           (line?RADAR_RX_SLICE_BYTES-RADAR_CACHE_LINE_BYTES:0U));
            for (k=0;k<16U;++k) { if (p[k]!=canary_word(slot,line,k)) { return RADAR_DMA_E_MEMORY; } }
        }
    }
    return 0;
}
/* Diagnostic READs use the existing mailbox owner. Two equal page passes are
 * only a consistency observation, never an atomic snapshot or a PHY test.
 * Pages 0B/0C are the retained tap SNAPSHOT copy, not live tap measurements. */
static void print_phy_status(RadarSw3 *s)
{
    enum { FLAGS, SYS_FAULT, PHY, PHY_GEN, PHY_FAULT, BASELINE, BASELINE_GEN,
           TAPS_COPY, TAPS_GEN, TAP_COPY_GEN, SNAPSHOT_GEN, IO, PAGE_COUNT };
    static const u8 pages[PAGE_COUNT]={
        RADAR_PAGE_FLAGS,RADAR_PAGE_SYS_FAULTS,RADAR_PAGE_PHY_STATUS,
        RADAR_PAGE_PHY_GEN,RADAR_PAGE_PHY_FAULT,RADAR_PAGE_BASELINE,
        RADAR_PAGE_BASELINE_GEN,0x0BU,0x0CU,RADAR_PAGE_TAP_COPY_GEN,
        RADAR_PAGE_SNAPSHOT_GEN,RADAR_PAGE_IO_REQUEST
    };
    static const char *const names[PAGE_COUNT]={
        "flags","sys_faults","phy_status","phy_terminal_gen","phy_fault",
        "baseline","baseline_gen","retained_taps","retained_tap_measurement_gen",
        "tap_copy_gen","rx_snapshot_gen","io_request"
    };
    u32 value[2][PAGE_COUNT]={{0U}},available[2]={0U,0U};
    u32 pass,i,consistent=1U;
    u64 start;
    int rc;
    RadarSw2 *sw2=s->sw2;
    if (s->job!=SW3_IDLE || sw2->state==SW2_CAPTURE ||
        sw2->job!=SW2_JOB_NONE || radar_cli_busy(&sw2->cli) ||
        (sw2->state!=SW2_SAFE && sw2->state!=SW2_ONLINE && sw2->state!=SW2_FAULT)) {
        SAY(s,"[REFUSE] phy status requires idle SW2/SW3 with no SW4 owner; no READ issued\r\n");return;
    }
    if (sw2->fatal_control || s->control->desynced || s->control->pending ||
        !s->control->initialized || !s->control->abi_verified) {
        SAY(s,"[REFUSE] phy status mailbox pending/desynced/unverified; no READ issued\r\n");return;
    }
    if (radar_console_free(s->console)<2048U) {
        SAY(s,"[REFUSE] phy status console busy; retry after output drains; no READ issued\r\n");return;
    }
    start=radar_hw_now_us();
    /* Exactly two passes; each read32 has its existing bounded ACK/rollover
     * checks. No SNAPSHOT, RECAL, reset, fault clear, config or UART TX. */
    for (pass=0U;pass<2U;++pass) {
        for (i=0U;i<PAGE_COUNT;++i) {
            rc=radar_control_read32(s->control,pages[i],&value[pass][i]);
            if (rc==RADAR_CONTROL_E_REMOTE &&
                ((s->control->last_response>>24U)&RADAR_CTRL_RESULT_MASK)==RADAR_GPIO_NOT_READY &&
                (i==BASELINE || i==TAPS_COPY || i==TAPS_GEN)) {
                continue; /* Defined unavailable data, NOT a zero measurement. */
            }
            if (rc!=0) {
                SAY(s,"[PHY LIVE] read_ok=0 consistent=0 atomic=0 failed_page=%02x error=%s; no data published\r\n",
                    (unsigned)pages[i],radar_control_status_name(rc));return;
            }
            available[pass]|=1U<<i;
            if (i==FLAGS && (value[pass][FLAGS]&RADAR_SW1_ACTIVITY_MASK)) {
                SAY(s,"[PHY LIVE] read_ok=0 consistent=0 atomic=0 PL_ACTIVE flags=%08x; no further READ issued\r\n",
                    (unsigned)value[pass][FLAGS]);return;
            }
        }
    }
    if (available[0]!=available[1]) { consistent=0U; }
    for (i=0U;i<PAGE_COUNT;++i) {
        if ((available[0]&(1U<<i)) && value[0][i]!=value[1][i]) { consistent=0U; }
    }
    SAY(s,"[PHY LIVE] read_ok=1 consistent=%u observation=%s passes=2 atomic=0 elapsed_us=%llu\r\n",
        (unsigned)consistent,consistent?"STABLE_TWO_PASSES":"CHANGED",
        (unsigned long long)(radar_hw_now_us()-start));
    SAY(s,"[PHY SCOPE] Second-pass values; consistency is not readiness or PHY/capture PASS. No measurement requested; faults not cleared.\r\n");
    for (i=0U;i<PAGE_COUNT;++i) {
        if (available[1]&(1U<<i)) {
            SAY(s,"[PHY PAGE] %02x %s=%08x\r\n",(unsigned)pages[i],names[i],(unsigned)value[1][i]);
        } else { SAY(s,"[PHY PAGE] %02x %s=N/A (NOT_READY)\r\n",(unsigned)pages[i],names[i]); }
    }
    SAY(s,"[PHY DECODE] result=%u phase=%u ref_ready=%u rx_ready=%u baseline_valid=%u arm_admissible=%u\r\n",
        (unsigned)(value[1][PHY]&15U),(unsigned)((value[1][PHY]>>4U)&7U),
        (unsigned)((value[1][PHY]>>8U)&1U),(unsigned)((value[1][PHY]>>9U)&1U),
        (unsigned)((value[1][PHY]>>7U)&1U),(unsigned)((value[1][PHY]>>10U)&1U));
    SAY(s,"[PHY RX] clock_alive=%u rx_ready=%u deser_resetn=%u fifo_resetn=%u\r\n",
        (unsigned)((value[1][FLAGS]>>7U)&1U),(unsigned)((value[1][FLAGS]>>5U)&1U),
        (unsigned)((value[1][FLAGS]>>6U)&1U),(unsigned)((value[1][FLAGS]>>8U)&1U));
    SAY(s,"[PHY TAPS] 0B/0C are the last successful tap snapshot copy; 13 is its copy generation. No SNAPSHOT means no new tap measurement.\r\n");
}
static int ensure_open(RadarSw3 *s)
{
    int rc=quiet_guard(s);
    if (rc) { SAY(s,"[REFUSE] SW3 requires SW2 SAFE, no IO/session/transaction; flags=%08x\r\n",(unsigned)s->guard_flags);return rc; }
    if (s->dma.state==RADAR_DMA_OFF) {
        rc=radar_dma_open(&s->dma);
        SAY(s,"[%s] DMA init: %s (no source started)\r\n",rc?"FAIL":"PASS",radar_dma_error_name(rc));
    } else if (s->dma.state!=RADAR_DMA_READY) {
        rc=RADAR_DMA_E_STATE;
        SAY(s,"[REFUSE] DMA=%s; dma status/trace; recovery is explicit\r\n",radar_dma_state_name(s->dma.state));
    }
    if (!rc && (!s->dma.ring_created || !s->dma.ring || !s->dma.reset_confirmed ||
        s->dma.lease_active || s->dma.packet_queued ||
        s->dma.ring->FreeCnt!=(int)RADAR_RX_BD_COUNT || s->dma.ring->PreCnt ||
        s->dma.ring->HwCnt || s->dma.ring->PostCnt ||
        !radar_dma_is_quiescent(&s->dma) || radar_dma_ring_check(&s->dma)!=0)) {
        SAY(s,"[REFUSE] DMA is not physically reset/all-free; no canary or BD memory written\r\n");
        rc=RADAR_DMA_E_OWNER;
    }
    return rc;
}
static void terminate_failure(RadarSw3 *s,int error)
{
    radar_sw3_revoke_qualification(s,s->canceled?RADAR_QUAL_CANCELED:RADAR_QUAL_TEST_FAILED);
    if (!s->check_error) { s->check_error=error; }
    if (!s->canceled) { ++s->checks_failed; }
    s->job=SW3_IDLE;
    SAY(s,"RESULT: %s SW3; error=%s halt=%s reset=%s; data remains owned until confirmed reset\r\n",
        s->canceled?"CANCELED":"FAIL",radar_dma_error_name(error),
        radar_dma_phase_name(s->dma.halt_result),radar_dma_phase_name(s->dma.reset_result));
    if (s->dma.state==RADAR_DMA_HALT_TIMEOUT) {
        SAY(s,"[QUARANTINE] RESET NOT ATTEMPTED. With source still never armed and SW2 SAFE, `dma reset` authorizes ONE reset. HALT stays failed.\r\n");
    } else if (s->dma.state==RADAR_DMA_LOCKED) {
        SAY(s,"[LOCKED] RESET completion not confirmed. No retry/free/requeue. Full target reset + PL/PS initialization required.\r\n");
    }
    radar_sw3_print_dma(s);
}
static void finish_check(RadarSw3 *s)
{
    int rc=s->check_error;
    if (!rc) { rc=verify_canaries(s); }
    if (!rc) { rc=quiet_guard(s); }
    if (!rc) { rc=radar_dma_ring_check(&s->dma); }
    if (!rc && (!s->run_observed || s->observed_completed_bds || s->observed_completed_packets ||
        s->dma.reset_result!=RADAR_DMA_PHASE_PASS ||
        (!s->direct_reset && s->dma.halt_result!=RADAR_DMA_PHASE_PASS))) { rc=RADAR_DMA_E_DEADLINE; }
    if (!rc && ((u32)s->dma.ring->FreeCnt!=RADAR_RX_BD_COUNT || s->dma.ring->PreCnt ||
        s->dma.ring->HwCnt || s->dma.ring->PostCnt || s->dma.lease_active)) { rc=RADAR_DMA_E_OWNER; }
    if (rc || s->canceled) { terminate_failure(s,rc?rc:RADAR_DMA_E_STATE);return; }
    if (s->direct_reset) { ++s->direct_reset_passes; }
    else { ++s->checks_passed; }
    ++s->batch_done;
    SAY(s,"RESULT: PASS SW3 %s %u/%u; halt=%s reset=%s; no input, 128 free BDs, four canary lines unchanged. REAL_RX=NOT_TESTED_BY_SW3\r\n",
        s->direct_reset?"RUN_TO_RESET_ONLY":"RUN_HALT_RESET",(unsigned)s->batch_done,(unsigned)s->batch_total,
        radar_dma_phase_name(s->dma.halt_result),radar_dma_phase_name(s->dma.reset_result));
    s->job=s->batch_done<s->batch_total?SW3_REPEAT_WAIT:SW3_IDLE;
    s->phase_us=radar_hw_now_us();
    /* An intermediate batch pass is history only. Issue at the FINAL epoch. */
    if (s->job==SW3_IDLE) {
        s->qualification_epoch=s->dma.epoch;
        s->qualification_reset_count=s->dma.reset_count;
        s->qualification_kind=s->direct_reset?RADAR_QUAL_RESET_CONTROL:RADAR_QUAL_RUN_HALT_RESET;
        s->qualification_reason=RADAR_QUAL_VALID;
        s->reset_qualified=1U;s->control_qualified=(u8)!s->direct_reset;
        radar_sw3_print_qualification(s);
    }
}
static int start_check(RadarSw3 *s)
{
    int rc;
    radar_sw3_revoke_qualification(s,RADAR_QUAL_NEW_TEST);
    rc=ensure_open(s);
    if (rc) { return rc; }
    s->check_error=0; s->canceled=0U;s->run_observed=0U;
    s->observed_completed_bds=0U;s->observed_completed_packets=0U;
    write_canaries(s);
    rc=radar_dma_prepare(&s->dma,CHECK_BYTES,1U,CHECK_SESSION,CHECK_LIMIT_US);
    if (rc==0) {
        s->expected_tail=(u32)(UINTPTR)s->dma.ring->HwTail;
        rc=radar_dma_start(&s->dma);
    }
    if (rc) { terminate_failure(s,rc);return rc; }
    s->job=SW3_WAIT_RUN;s->phase_us=radar_hw_now_us();s->guard_us=s->phase_us;
    SAY(s,"[RUN] SG control: two 64KiB BDs, 20ms no-input window, mode=%s. No ARM/sensorStart.\r\n",
        s->direct_reset?"RUN_TO_RESET":"RUN_HALT_RESET");
    return 0;
}
void radar_sw3_init(RadarSw3 *s,RadarSw2 *sw2,RadarControl *control,RadarConsole *console)
{
    memset(s,0,sizeof(*s));s->sw2=sw2;s->control=control;s->console=console;radar_dma_construct(&s->dma);
    SAY(s,"[SW3.2] dma qualify / qualify10 (RUN->RESET) | dma check / check10 (HALT diagnostic) | dma reset/status/trace | progress\r\n");
}
void radar_sw3_poll(RadarSw3 *s)
{
    u64 now=radar_hw_now_us();
    RadarDmaRegisters r;
    int rc=radar_dma_poll(&s->dma,now);
    if (s->job==SW3_IDLE) { expire_qualification(s);return; }
    if (s->dma.state==RADAR_DMA_LOCKED || s->dma.state==RADAR_DMA_HALT_TIMEOUT || rc<0) {
        terminate_failure(s,rc<0?rc:s->dma.last_error);return;
    }
    if (s->job==SW3_RESET_ONLY) {
        if (s->dma.state==RADAR_DMA_READY) {
            if (s->dma.reset_result==RADAR_DMA_PHASE_PASS) { ++s->reset_only_passes; }
            SAY(s,"[RESET] %s; ring rebuilt after real quiescence. Previous halt=%s is NOT promoted to PASS.\r\n",
                radar_dma_phase_name(s->dma.reset_result),radar_dma_phase_name(s->dma.halt_result));
            s->job=SW3_IDLE;
        }
        return;
    }
    if (s->job==SW3_WAIT_RESET) { if (s->dma.state==RADAR_DMA_READY) { finish_check(s); }return; }
    if (s->job==SW3_REPEAT_WAIT) {
        if (now-s->phase_us>=100000U && radar_console_free(s->console)>4096U) {
            if (start_check(s)!=0) { s->job=SW3_IDLE; }
        }
        return;
    }
    if (now-s->guard_us>=1000U) {
        s->guard_us=now;
        rc=quiet_guard(s);
        if (rc) { (void)radar_dma_stop_begin(&s->dma);terminate_failure(s,rc);return; }
    }
    if (s->job==SW3_WAIT_RUN || s->job==SW3_OBSERVE) {
        s->observed_completed_bds=s->dma.completed_bds;s->observed_completed_packets=s->dma.completed_packets;
        if (s->dma.completed_bds || s->dma.partial_prefix) {
            (void)radar_dma_stop_begin(&s->dma);terminate_failure(s,RADAR_DMA_E_BOUNDARY);return;
        }
        rc=radar_dma_probe(&r);
        if (rc) { terminate_failure(s,rc);return; }
        if (s->job==SW3_WAIT_RUN && !(r.status&XAXIDMA_HALTED_MASK) && (r.control&XAXIDMA_CR_RUNSTOP_MASK)) {
            if ((UINTPTR)r.current<s->dma.bd_base || (UINTPTR)r.current>=s->dma.bd_base+s->dma.ring->Length ||
                ((UINTPTR)r.current-s->dma.bd_base)%s->dma.ring->Separation || r.tail!=s->expected_tail) {
                (void)radar_dma_stop_begin(&s->dma);terminate_failure(s,RADAR_DMA_E_DESCRIPTOR);return;
            }
            s->run_observed=1U;s->job=SW3_OBSERVE;s->phase_us=now;
            SAY(s,"[PASS] SG RUN observed CR=%08x SR=%08x CUR=%08x TAIL=%08x\r\n",
                (unsigned)r.control,(unsigned)r.status,(unsigned)r.current,(unsigned)r.tail);
        } else if (s->job==SW3_OBSERVE && now-s->phase_us>=CHECK_TIME_US) {
            rc=s->direct_reset?radar_dma_reset_begin(&s->dma):radar_dma_stop_begin(&s->dma);
            if (rc<0) { terminate_failure(s,rc);return; }
            s->job=s->direct_reset?SW3_WAIT_RESET:SW3_WAIT_HALT;
        }
    } else if (s->job==SW3_WAIT_HALT && s->dma.state==RADAR_DMA_HALTED) {
        if (s->dma.halt_result!=RADAR_DMA_PHASE_PASS) { s->check_error=RADAR_DMA_E_DEADLINE; }
        SAY(s,"[HALT] %s; full reset follows, preserving phase result\r\n",radar_dma_phase_name(s->dma.halt_result));
        rc=radar_dma_reset_begin(&s->dma);
        if (rc<0) { terminate_failure(s,rc);return; }
        s->job=SW3_WAIT_RESET;
    }
}
static int parse_plan(const char *s,u32 *a,u32 *b)
{
    u32 *dst[2]={a,b};unsigned k;
    for (k=0;k<2U;++k) {
        u64 v=0U;while (*s==' ' || *s=='\t') { ++s; }
        if (*s<'0'||*s>'9') { return 0; }
        do { v=v*10U+(u32)(*s-'0');if(v>UINT32_MAX){return 0;}++s; } while (*s>='0'&&*s<='9');
        *dst[k]=(u32)v;if(k==0U&&*s!=' '&&*s!='\t'){return 0;}
    }
    while (*s==' '||*s=='\t') { ++s; }return *s=='\0';
}
int radar_sw3_command(RadarSw3 *s,const char *cmd)
{
    int rc;
    if (strcmp(cmd,"help")==0) {
        SAY(s,"SW3.2: dma init/status/trace/reset | dma qualify/qualify10 | dma check/check10 (HALT diagnostic) | dma check reset / dma check10 reset | dma plan BYTES PACKETS | phy status (READ only)\r\n");return 0;
    }
    if (strcmp(cmd,"progress")==0) {
        SAY(s,"[PROGRESS boot] SW0/SW1=PASS SW2 check10=%u reset3=%u SW3 run_halt_reset=%u direct_reset=%u failed=%u\r\n",
            (unsigned)s->sw2->check10_passes,(unsigned)s->sw2->reset3_passes,(unsigned)s->checks_passed,
            (unsigned)s->direct_reset_passes,(unsigned)s->checks_failed);
        radar_sw3_print_qualification(s);
        SAY(s,"[SCOPE] SW3 proves control ONLY. Actual RAW16 result belongs to `capture status`; ADC semantics are not validated here.\r\n");return 1;
    }
    if (strcmp(cmd,"dma status")==0) { radar_sw3_print_dma(s);return 1; }
    if (strcmp(cmd,"dma trace")==0) { print_trace(s);return 1; }
    if (strncmp(cmd,"dma plan ",9U)==0) {
        u32 bytes,packets,bds=0U,prime=0U;
        if (!parse_plan(cmd+9,&bytes,&packets)) { SAY(s,"Usage: dma plan BYTES PACKETS (decimal; 0=continuous planning ONLY)\r\n");return 1; }
        rc=radar_dma_plan(bytes,packets,&bds,&prime);
        SAY(s,"[PLAN] %s prime_packets=%u BDs=%u; does not modify dma check or capture profile\r\n",radar_dma_error_name(rc),(unsigned)prime,(unsigned)bds);return 1;
    }
    if (strcmp(cmd,"safe")==0||strcmp(cmd,"disable")==0) {
        if (s->job!=SW3_IDLE) {
            s->canceled=1U;radar_sw3_revoke_qualification(s,RADAR_QUAL_CANCELED);
            if (s->dma.state==RADAR_DMA_RUNNING || s->dma.state==RADAR_DMA_STAGED) { (void)radar_dma_stop_begin(&s->dma); }
            s->job=SW3_IDLE;
            SAY(s,"[CANCELED] SW3. Halt is polled, no automatic reset. Use dma status then explicit dma reset.\r\n");
        }
        return 0;
    }
    if (s->job!=SW3_IDLE) { SAY(s,"[BUSY] SW3 check/reset; dma status/trace/progress/safe remain available\r\n");return 1; }
    if (strcmp(cmd,"phy status")==0) { print_phy_status(s);return 1; }
    if (strcmp(cmd,"dma init")==0) { if(ensure_open(s)==0){radar_sw3_print_dma(s);}return 1; }
    if (strcmp(cmd,"dma reset")==0) {
        if (quiet_guard(s)!=0) { SAY(s,"[REFUSE] Explicit DMA reset needs SW2 SAFE and no PL producer/session\r\n");return 1; }
        radar_sw3_revoke_qualification(s,RADAR_QUAL_EXPLICIT_RESET);
        rc=radar_dma_reset_authorized(&s->dma);
        if (rc<0) { SAY(s,"[REFUSE] reset: %s; RESET_LOCKED is not retried\r\n",radar_dma_error_name(rc)); }
        else { s->canceled=0U;s->check_error=0;s->job=SW3_RESET_ONLY;SAY(s,"[RESET] One explicit reset request; DDR remains quarantined until confirmed\r\n"); }
        return 1;
    }
    if (strcmp(cmd,"dma check")==0 || strcmp(cmd,"dma check10")==0 ||
        strcmp(cmd,"dma check reset")==0 || strcmp(cmd,"dma check10 reset")==0 ||
        strcmp(cmd,"dma qualify")==0 || strcmp(cmd,"dma qualify10")==0) {
        s->direct_reset=(u8)(strcmp(cmd,"dma check")!=0 && strcmp(cmd,"dma check10")!=0);
        s->batch_total=(strcmp(cmd,"dma check10")==0 || strcmp(cmd,"dma check10 reset")==0 ||
                        strcmp(cmd,"dma qualify10")==0)?10U:1U;s->batch_done=0U;
        (void)start_check(s);return 1;
    }
    if (strncmp(cmd,"dma",3U)==0&&(cmd[3]=='\0'||cmd[3]==' ')) { SAY(s,"[REJECT] Unknown dma command; type help\r\n");return 1; }
    if (s->dma.state!=RADAR_DMA_OFF&&s->dma.state!=RADAR_DMA_READY&&
        strcmp(cmd,"status")&&strcmp(cmd,"profile")&&strcmp(cmd,"response")) {
        SAY(s,"[REFUSE] DMA=%s; no new radar IO until quiescent/reset\r\n",radar_dma_state_name(s->dma.state));return 1;
    }
    return 0;
}
