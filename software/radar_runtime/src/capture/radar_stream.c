/* SPDX-License-Identifier: MIT
 * Single cooperative STREAM producer. No cyclic DMA, implicit restart, or
 * interpretation of a transport packet as valid ADC until strict HSI checks.
 */
#include "radar_stream.h"
#include "radar_crc32.h"
#include "radar_memory.h"
#include "radar_hw.h"
#include "xil_cache.h"
#include <string.h>
#define PG (RADAR_FLAG_PG|RADAR_FLAG_PG_QUALIFIED)
#define COPY_QUANTUM 8192U
#define PHASE_TIMEOUT_US 500000U
static int exec(RadarSession *s,u8 op,u16 arg)
{ return radar_control_exec(s->control,op,0U,arg,NULL); }
static int page(RadarSession *s,u8 p,u32 *v)
{ return radar_control_read32(s->control,p,v); }
static void enter(RadarSession *s,RadarSessionState state,u64 now)
{ s->state=state;s->phase_us=now;s->phase_polls=0U; }
static int cli_start(RadarSession *s,RadarCaptureCommand c,u32 index,u64 now)
{ return radar_cli_start_capture(&s->sw2->cli,s->profile,c,index,now); }
static int cli_result(RadarSession *s)
{ int rc;if(!radar_cli_take_result(&s->sw2->cli,&rc))return 1;return rc; }
static int health(RadarSession *s)
{
    int rc;u32 want=s->sw2->expected_io;
    rc=page(s,RADAR_PAGE_FLAGS,&s->flags);if(rc)return rc;
    rc=page(s,RADAR_PAGE_IO_REQUEST,&s->io_request);if(rc)return rc;
    rc=page(s,RADAR_PAGE_SYS_FAULTS,&s->sys_faults);if(rc)return rc;
    rc=page(s,RADAR_PAGE_PHY_STATUS,&s->phy_status);if(rc)return rc;
    rc=page(s,RADAR_PAGE_PHY_FAULT,&s->phy_fault);if(rc)return rc;
    if((s->flags&PG)!=PG || (s->flags&RADAR_FLAG_POWER_LOST) || !(s->flags&RADAR_FLAG_NERR_N))return RADAR_SESSION_E_POWER;
    if(s->io_request!=want || (want==3U && (s->flags&RADAR_SW1_OUTPUT_MASK)!=(RADAR_FLAG_UART_OE|RADAR_FLAG_SYNC_OE)) ||
       (want==0U && (s->flags&RADAR_SW1_OUTPUT_MASK)))return RADAR_SESSION_E_OWNER;
    if(s->state>=RADAR_SESSION_CONFIG && s->state<=RADAR_SESSION_COPY &&
       (s->sys_faults || (s->flags&RADAR_FLAG_RX_FAULT)))return RADAR_SESSION_E_DATA;
    if(s->state>=RADAR_SESSION_SHADOW && s->state<=RADAR_SESSION_SNAPSHOT && s->phy_fault)return RADAR_SESSION_E_PHY;
    if(s->state>=RADAR_SESSION_CONFIG && s->state<=RADAR_SESSION_SHADOW &&
       (s->flags&(RADAR_FLAG_SESSION_OPEN|RADAR_FLAG_ARM_PENDING|RADAR_FLAG_CMD_PENDING)))return RADAR_SESSION_E_OWNER;
    return 0;
}
static int recovered(RadarSession *s)
{
    int rc=page(s,RADAR_PAGE_FLAGS,&s->flags);if(rc)return rc;
    if(s->flags&(RADAR_FLAG_RECOVERING|RADAR_FLAG_SESSION_OPEN|RADAR_FLAG_CMD_PENDING|
       RADAR_FLAG_ARM_PENDING|RADAR_FLAG_SNAPSHOT_PENDING|RADAR_FLAG_PHY_BUSY))return 1;
    if(!(s->flags&RADAR_FLAG_DMA_RESETN))return 1;
    rc=page(s,RADAR_PAGE_SYS_FAULTS,&s->sys_faults);if(rc)return rc;return s->sys_faults?-1:0;
}
static void failed(RadarStream *r,int error,int native)
{
    RadarSession *s=r->session;RadarDma *d=&s->sw3->dma;
    r->error=error;r->native_error=native;
    if(d->partial_prefix)r->frames_residual=1U;
    if(r->copy_lease && radar_dma_release(d,&r->copy_packet)==0)r->copy_lease=0U;
    if(r->writing){(void)radar_raw_slots_discard_write(&r->raw,r->writing);r->writing=NULL;}
    /* The established supervisor isolates outputs, requests ABORT/HALT and
     * quarantines. It never claims RF stopped or performs a blind reset. */
    radar_session_cancel(s);s->error=error;s->native_error=native;r->active=0U;
    (void)radar_console_printf(s->console,"[STREAM FAIL] error=%d native=%d verified=%u residual=%u; explicit recovery required\r\n",
        error,native,(unsigned)r->frames_verified,(unsigned)r->frames_residual);
}
void radar_stream_init(RadarStream *r,RadarSession *s,const RadarStreamIo *io)
{
    memset(r,0,sizeof(*r));r->session=s;if(io)r->io=*io;
    radar_raw_slots_init(&r->raw,radar_memory_capture_base());
}
int radar_stream_active(const RadarStream *r){return r && r->active;}
int radar_stream_display_idle(const RadarStream *r)
{
    const RadarSession *s;
    if(!r || !r->active)return 0;
    s=r->session;
    if(radar_session_display_idle(s))return 1;
    /* STREAM keeps DMA running during normal rendering. Display reads its own
     * immutable snapshot; ring/RAW leases cover the cooperative render slice. */
    return (s->state==RADAR_SESSION_RECEIVE || s->state==RADAR_SESSION_COPY) &&
        !s->control->pending && !s->control->desynced && !s->sw2->fatal_control &&
        !radar_cli_busy(&s->sw2->cli) && !s->sw2->cli.terminal && !s->sw2->cli.needs_reset;
}
int radar_stream_acquire(RadarStream *r,RadarStreamFrame *f)
{ return r?radar_raw_slots_acquire(&r->raw,f):-1; }
int radar_stream_release(RadarStream *r,const RadarStreamFrame *f)
{ int rc;if(!r)return -1;rc=radar_raw_slots_release(&r->raw,f);if(!rc)++r->frames_released;return rc; }
int radar_stream_start(RadarStream *r,u32 limit)
{
    RadarSession *s;RadarDma *d;int rc;
    if(!r || !(s=r->session))return RADAR_SESSION_E_OWNER;
    d=&s->sw3->dma;
    if(r->active || r->metadata_open || r->raw.used || s->profile!=radar_capture_profile_stream(limit) ||
       !s->profile || s->profile->qualification!=RADAR_CAPTURE_STREAM_FRAME_TEST)return RADAR_SESSION_E_PROFILE;
    if(!r->io.validate_frame || !r->io.transport_begin || !r->io.transport_pop || !r->io.transport_end)
        return RADAR_STREAM_E_CAPABILITY;
    if(radar_session_preflight(s))return RADAR_SESSION_E_PREREQUISITE;
    if(d->state!=RADAR_DMA_READY || !d->reset_confirmed || !radar_dma_is_quiescent(d) ||
       d->ring->FreeCnt!=(int)RADAR_RX_BD_COUNT)return RADAR_SESSION_E_DMA;
    rc=radar_sw2_capture_claim(s->sw2);if(rc)return rc;
    radar_session_invalidate_config(s);s->owns=1U;s->active_session=s->next_session++;
    s->begin_us=radar_hw_now_us();s->guard_us=s->begin_us;s->admission_epoch=d->epoch;
    s->admission_kind=s->sw3->qualification_kind;s->source=RADAR_SOURCE_UNKNOWN;
    s->config_index=0U;s->shadow_index=0U;s->error=0;s->native_error=0;s->cleanup_error=0;
    s->result.valid=0U;s->reported_failure=0U;memset(&s->phy,0,sizeof(s->phy));
    r->frame_limit=limit;r->frames_verified=0U;r->frames_released=0U;r->frames_residual=0U;
    r->metadata_received=0U;memset(r->metadata_sequence,0,sizeof(r->metadata_sequence));
    r->copy_lease=0U;r->writing=NULL;r->copy_done_us=0U;r->error=0;r->native_error=0;
    r->stop_requested=0U;r->stop_sent=0U;r->boundary_seen=0U;r->stop_target=0U;r->stop_reason=0U;
    r->stop_by_reset=0U;r->stop_unused_bds=0U;
    r->max_backlog=0U;r->copy_cpu_us=0U;r->health_cpu_us=0U;r->health_max_us=0U;
    r->last_progress_us=s->begin_us;r->active=1U;
    radar_sw3_consume_qualification(s->sw3);enter(s,RADAR_SESSION_STOP_BEFORE,s->begin_us);
    rc=cli_start(s,RADAR_CAPTURE_CMD_STOP,0U,s->begin_us);
    if(rc){failed(r,RADAR_SESSION_E_UART,rc);return rc;}return 0;
}
int radar_stream_request_stop(RadarStream *r,int reason)
{
    if(!r || !r->active)return RADAR_SESSION_E_OWNER;
    r->stop_requested=1U;if(reason && !r->stop_reason)r->stop_reason=reason;return 0;
}
void radar_stream_cancel(RadarStream *r)
{ if(r && r->active)failed(r,RADAR_SESSION_E_CANCELED,0); }
int radar_stream_clear_fault(RadarStream *r)
{
    RadarSession *s;int rc;
    if(!r || !(s=r->session) || r->active || s->owns || s->state!=RADAR_SESSION_IDLE ||
       s->source!=RADAR_SOURCE_STOPPED || s->sw3->dma.state!=RADAR_DMA_READY || r->raw.used)return -1;
    if(r->metadata_open){rc=r->io.transport_end(r->io.context,s->active_session);if(rc)return rc;r->metadata_open=0U;}
    r->error=0;r->native_error=0;return 0;
}
static int metadata_step(RadarStream *r)
{
    RadarTransportTime t;u64 first;u32 error,index;int rc;
    if(!r->metadata_open)return 0;
    rc=r->io.transport_pop(r->io.context,&t,&first,&error);if(rc<=0)return rc;
    if(!t.valid || t.sequence!=r->metadata_received+1U || t.bytes!=RADAR_STREAM_FRAME_BYTES ||
       t.last_tick<t.first_tick)return RADAR_STREAM_E_METADATA;
    index=t.sequence%32U;if(r->metadata_sequence[index])return RADAR_STREAM_E_METADATA;
    r->metadata[index]=t;r->metadata_sequence[index]=t.sequence;
    r->metadata_first_us[index]=first;r->metadata_error_us[index]=error;++r->metadata_received;return 0;
}
static int copy_slice(RadarStream *r,u64 now)
{
    RadarSession *s=r->session;RadarDma *d=&s->sw3->dma;RadarDmaFragment f;u32 n,index;int rc;
    if(!r->copy_lease) {
        if(!d->packet_queued)return 0;
        rc=radar_dma_acquire(d,&r->copy_packet);if(rc)return rc;
        r->copy_lease=1U;
        if(r->copy_packet.session!=s->active_session || r->copy_packet.epoch!=s->admission_epoch ||
           r->copy_packet.sequence!=r->frames_verified+1U || r->copy_packet.bytes!=RADAR_STREAM_FRAME_BYTES)
            return RADAR_SESSION_E_DATA;
        rc=radar_raw_slots_reserve(&r->raw,s->active_session,r->copy_packet.sequence,d->epoch,
            r->copy_packet.observed_us,&r->writing);if(rc)return RADAR_STREAM_E_OVERLOAD;
        r->copy_fragment=0U;r->copy_offset=0U;r->copy_bytes=0U;r->copy_crc=UINT32_MAX;r->copy_done_us=0U;
        r->copy_cpu_us=0U;
    }
    if(r->copy_bytes<RADAR_STREAM_FRAME_BYTES) {
        rc=radar_dma_fragment(d,&r->copy_packet,r->copy_fragment,&f);if(rc)return rc;
        n=f.bytes-r->copy_offset;if(n>COPY_QUANTUM)n=COPY_QUANTUM;
        if(!n || r->copy_bytes+n>RADAR_STREAM_FRAME_BYTES)return RADAR_SESSION_E_MEMORY;
        memcpy((void *)(r->writing->raw+r->copy_bytes),(const void *)(f.address+r->copy_offset),n);
        r->copy_crc=radar_crc32_update(r->copy_crc,(const void *)(r->writing->raw+r->copy_bytes),n);
        r->copy_bytes+=n;r->copy_offset+=n;
        if(r->copy_offset==f.bytes){r->copy_offset=0U;++r->copy_fragment;}
        if(r->copy_bytes<RADAR_STREAM_FRAME_BYTES)return 0;
        r->copy_done_us=now;
        rc=r->io.validate_frame(r->io.context,(const void *)r->writing->raw,RADAR_STREAM_FRAME_BYTES,&r->writing->hsi_id);
        if(rc)return RADAR_STREAM_E_HSI;
    }
    index=r->copy_packet.sequence%32U;
    if(r->metadata_sequence[index]!=r->copy_packet.sequence)
        return now-r->copy_done_us>20000U?RADAR_STREAM_E_METADATA:0;
    r->writing->transport=r->metadata[index];r->writing->first_accept_us=r->metadata_first_us[index];
    r->writing->time_error_us=r->metadata_error_us[index];r->writing->crc32=r->copy_crc^UINT32_MAX;
    r->writing->verified_us=now;
    /* This is a CPU-only immutable store; no whole-frame cache operation in
     * the receive loop. The processor flushes its separate PL input slot. */
    radar_hw_barrier();rc=radar_raw_slots_publish(&r->raw,r->writing);if(rc)return rc;
    r->writing=NULL;r->metadata_sequence[index]=0U;++r->frames_verified;
    rc=radar_dma_release(d,&r->copy_packet);if(rc)return rc;r->copy_lease=0U;r->last_progress_us=now;
    if(!r->stop_requested && !r->boundary_seen && (!r->frame_limit || d->submitted_packets<r->frame_limit)) {
        rc=radar_dma_refill(d,1U);if(rc)return rc;
    }
    return 0;
}
static int copy_step(RadarStream *r,u64 now)
{
    u64 begin=radar_hw_now_us();u32 previous=r->frames_verified,i;int rc=copy_slice(r,now);
    r->copy_cpu_us+=(u32)(radar_hw_now_us()-begin);
    if(r->frames_verified!=previous)for(i=0U;i<RADAR_RAW_SLOT_COUNT;++i) {
        RadarRawSlot *slot=&r->raw.slots[i];
        if(slot->state==RADAR_RAW_VERIFIED && slot->frame.capture_epoch==r->session->active_session &&
           slot->frame.transport_sequence==r->frames_verified){slot->frame.copy_cpu_us=r->copy_cpu_us;break;}
    }
    return rc;
}
static int snapshot_check(RadarStream *r)
{
    RadarSnapshotData *d=&r->session->snapshot.data;u64 bytes=(u64)d->words[4]*RADAR_STREAM_FRAME_BYTES;
    u32 wanted=RADAR_FLAG_SESSION_OPEN|RADAR_FLAG_SOURCE_COMPLETE|RADAR_FLAG_SNAPSHOT_VALID|RADAR_FLAG_DMA_RESETN;
    if(d->words[4])wanted|=RADAR_FLAG_FIRST;
    if((((u64)d->words[1]<<32U)|d->words[0])!=bytes/4U ||
       (((u64)d->words[3]<<32U)|d->words[2])!=bytes/8U || d->words[6]!=r->session->active_session ||
       (d->words[7]&0xffffU) || ((d->words[7]>>16U)&7U)!=4U || d->words[9] || d->words[5]>8192U ||
       d->words[11] || (d->words[10]&((1U<<16U)|(1U<<5U))) || ((d->words[10]>>11U)&31U)!=8U ||
       (d->flags&wanted)!=wanted || (d->flags&(RADAR_FLAG_RX_FAULT|RADAR_FLAG_ARM_PENDING|RADAR_FLAG_CMD_PENDING)) ||
       d->sys_faults || d->phy_fault || (r->frame_limit && !r->stop_requested && d->words[4]!=r->frame_limit))return -1;
    r->stop_target=d->words[4];return 0;
}
static int stop_drain_proof(RadarStream *r)
{
    RadarSession *s=r->session;RadarDma *d=&s->sw3->dma;u32 i;int rc;
    const u32 flags=RADAR_FLAG_SOURCE_COMPLETE|RADAR_FLAG_SESSION_OPEN|RADAR_FLAG_DMA_RESETN;
    /* A partial-prefix count of zero alone says nothing about outstanding
     * AXI transactions. These checks establish a stopped source and ownership
     * boundary; native software ResetIsDone + Halted remains the completion
     * barrier before any descriptor memory is reconstructed or reused. */
    if(!r->boundary_seen || !r->metadata_open || s->source!=RADAR_SOURCE_STOPPED || !s->owns ||
       (s->admission_kind!=RADAR_QUAL_RESET_CONTROL && s->admission_kind!=RADAR_QUAL_RUN_HALT_RESET) ||
       d->state!=RADAR_DMA_RUNNING || d->epoch!=s->admission_epoch || d->last_error ||
       r->copy_lease || r->writing || d->lease_active || d->partial_prefix || d->packet_queued ||
       d->completed_packets!=r->stop_target || d->released_packets!=r->stop_target ||
       r->frames_verified!=r->stop_target || r->metadata_received!=r->stop_target ||
       d->completed_bytes!=(u64)r->stop_target*RADAR_STREAM_FRAME_BYTES ||
       (u64)d->completed_bds!=(u64)r->stop_target*d->fragments_per_packet ||
       d->released_bds!=d->completed_bds || d->submitted_bds<d->completed_bds ||
       d->ring->PreCnt || d->ring->PostCnt ||
       d->ring->HwCnt!=(int)(d->submitted_bds-d->completed_bds) ||
       d->ring->HwCnt+d->ring->FreeCnt!=(int)RADAR_RX_BD_COUNT)return RADAR_SESSION_E_DATA;
    for(i=0U;i<32U;++i)if(r->metadata_sequence[i])return RADAR_STREAM_E_METADATA;
    /* Re-read current power/control/source flags after sensorStop ACK; the
     * earlier immutable snapshot established exact PL packet and byte totals. */
    rc=health(s);if(rc)return rc;
    if((s->flags&flags)!=flags ||
       (s->flags&(RADAR_FLAG_ARM_PENDING|RADAR_FLAG_CMD_PENDING|RADAR_FLAG_RX_FAULT)))return RADAR_SESSION_E_DATA;
    return 0;
}
void radar_stream_poll(RadarStream *r)
{
    RadarSession *s;RadarDma *d;const RadarCaptureProfile *p;u64 now;int rc;
    if(!r || !r->active)return;
    s=r->session;d=&s->sw3->dma;p=s->profile;now=radar_hw_now_us();
    rc=radar_dma_poll(d,now);radar_cli_poll(&s->sw2->cli,now);
    if(rc<0){failed(r,RADAR_SESSION_E_DMA,rc);return;}
    if(now<s->guard_us){failed(r,RADAR_SESSION_E_TIMEOUT,0);return;}
    if(now-s->guard_us>=1000U){
        u64 at=radar_hw_now_us();u32 used;s->guard_us=now;rc=health(s);
        used=(u32)(radar_hw_now_us()-at);r->health_cpu_us+=used;if(used>r->health_max_us)r->health_max_us=used;
        if(rc){failed(r,rc,0);return;}
    }
    if(s->state!=RADAR_SESSION_RECEIVE && ++s->phase_polls>10000000U){failed(r,RADAR_SESSION_E_TIMEOUT,0);return;}
    if(r->metadata_open && s->state<=RADAR_SESSION_COPY){rc=metadata_step(r);if(rc){failed(r,RADAR_STREAM_E_METADATA,rc);return;}}
    if(s->state>=RADAR_SESSION_START && s->state<=RADAR_SESSION_COPY) {
        u32 backlog=r->raw.ready+d->packet_queued;u64 oldest=radar_raw_slots_oldest_us(&r->raw);
        if(d->packet_queued && d->packet[d->packet_head].observed_us<oldest)oldest=d->packet[d->packet_head].observed_us;
        if(backlog>r->max_backlog)r->max_backlog=backlog;
        if(!r->stop_requested && (backlog>=8U || (oldest!=UINT64_MAX && (now<oldest || now-oldest>500000U))))
            (void)radar_stream_request_stop(r,backlog>=8U?RADAR_STREAM_E_OVERLOAD:RADAR_STREAM_E_STALE);
        rc=copy_step(r,now);if(rc){failed(r,rc,0);return;}
    }
    switch(s->state) {
    case RADAR_SESSION_STOP_BEFORE:
        rc=cli_result(s);if(rc==1)break;if(rc){failed(r,RADAR_SESSION_E_UART,rc);break;}
        s->source=RADAR_SOURCE_STOPPED;rc=exec(s,RADAR_OP_RECOVER,0xC1EAU);
        if(rc){failed(r,RADAR_SESSION_E_RECOVERY,rc);break;}enter(s,RADAR_SESSION_RECOVER_BEFORE,now);break;
    case RADAR_SESSION_RECOVER_BEFORE:
        rc=recovered(s);if(rc==1){if(now-s->phase_us>100000U)failed(r,RADAR_SESSION_E_TIMEOUT,0);break;}
        if(rc){failed(r,RADAR_SESSION_E_RECOVERY,rc);break;}
        rc=cli_start(s,RADAR_CAPTURE_CMD_CONFIG,0U,now);if(rc){failed(r,RADAR_SESSION_E_UART,rc);break;}
        enter(s,RADAR_SESSION_CONFIG,now);break;
    case RADAR_SESSION_CONFIG:
        if(s->sw2->cli.state==RADAR_CLI_IDLE && !s->sw2->cli.terminal) {
            rc=cli_start(s,RADAR_CAPTURE_CMD_CONFIG,s->config_index,now);if(rc)failed(r,RADAR_SESSION_E_UART,rc);break;
        }
        rc=cli_result(s);if(rc==1)break;if(rc){failed(r,RADAR_SESSION_E_UART,rc);break;}
        if(++s->config_index<p->command_count)break;
        rc=radar_phy_begin(&s->phy,s->control,RADAR_OP_RECAL,now);if(rc){failed(r,RADAR_SESSION_E_PHY,rc);break;}
        enter(s,RADAR_SESSION_RECAL,now);break;
    case RADAR_SESSION_RECAL:
        rc=radar_phy_poll(&s->phy,s->control,now);if(rc==1)break;if(rc){failed(r,RADAR_SESSION_E_PHY,rc);break;}
        enter(s,RADAR_SESSION_SHADOW,now);break;
    case RADAR_SESSION_SHADOW: {
        u32 value=s->shadow_index==0U?p->packet_bytes/4U:s->shadow_index==1U?r->frame_limit:
            s->shadow_index==2U?0U:s->shadow_index==3U?s->active_session:UINT32_C(0x16160008);
        rc=radar_control_write_config(s->control,(u8)s->shadow_index,value);if(rc){failed(r,RADAR_SESSION_E_GPIO,rc);break;}
        if(++s->shadow_index<5U)break;
        rc=radar_dma_prepare_window(d,p->packet_bytes,r->frame_limit,s->active_session,p->receive_timeout_us);
        if(!rc)rc=radar_dma_start(d);
        if(rc){failed(r,RADAR_SESSION_E_DMA,rc);break;}
        enter(s,RADAR_SESSION_DMA_RUNNING,now);break; }
    case RADAR_SESSION_DMA_RUNNING: {
        RadarDmaRegisters regs;rc=radar_dma_probe(&regs);if(rc){failed(r,RADAR_SESSION_E_DMA,rc);break;}
        if((regs.status&XAXIDMA_HALTED_MASK) || !(regs.control&XAXIDMA_CR_RUNSTOP_MASK))break;
        rc=page(s,RADAR_PAGE_PHY_STATUS,&s->phy_status);if(rc || !(s->phy_status&RADAR_PHY_ARM_ADMISSIBLE)){failed(r,RADAR_SESSION_E_PHY,rc);break;}
        rc=r->io.transport_begin(r->io.context,s->active_session);if(rc){failed(r,RADAR_STREAM_E_CAPABILITY,rc);break;}
        r->metadata_open=1U;s->armed_us=now;rc=exec(s,RADAR_OP_ARM,0U);if(rc){failed(r,RADAR_SESSION_E_GPIO,rc);break;}
        s->source=RADAR_SOURCE_STARTING;rc=cli_start(s,RADAR_CAPTURE_CMD_START,0U,now);
        if(rc){failed(r,RADAR_SESSION_E_UART,rc);break;}enter(s,RADAR_SESSION_START,now);break; }
    case RADAR_SESSION_START:
        rc=cli_result(s);if(rc==1)break;if(rc){failed(r,RADAR_SESSION_E_UART,rc);break;}
        s->source=RADAR_SOURCE_RUNNING;enter(s,RADAR_SESSION_RECEIVE,now);break;
    case RADAR_SESSION_RECEIVE:
        if(r->stop_requested && !r->stop_sent) {
            rc=exec(s,RADAR_OP_STOP,0U);if(rc){failed(r,RADAR_SESSION_E_GPIO,rc);break;}
            r->stop_sent=1U;r->stop_us=now;
        }
        if(r->stop_sent && now-r->stop_us>PHASE_TIMEOUT_US){failed(r,RADAR_SESSION_E_TIMEOUT,0);break;}
        rc=page(s,RADAR_PAGE_FLAGS,&s->flags);if(rc){failed(r,RADAR_SESSION_E_GPIO,rc);break;}
        if(!(s->flags&RADAR_FLAG_SOURCE_COMPLETE))break;
        r->boundary_seen=1U;radar_snapshot_begin(&s->snapshot,now,p->snapshot_timeout_us);
        enter(s,RADAR_SESSION_SNAPSHOT,now);break;
    case RADAR_SESSION_SNAPSHOT:
        rc=radar_snapshot_poll(&s->snapshot,s->control,now);if(rc==1)break;
        if(rc || snapshot_check(r)){failed(r,RADAR_SESSION_E_DATA,rc);break;}
        rc=cli_start(s,RADAR_CAPTURE_CMD_STOP,0U,now);if(rc){failed(r,RADAR_SESSION_E_UART,rc);break;}
        enter(s,RADAR_SESSION_STOP_AFTER,now);break;
    case RADAR_SESSION_STOP_AFTER:
        rc=cli_result(s);if(rc==1)break;if(rc){failed(r,RADAR_SESSION_E_UART,rc);break;}
        s->source=RADAR_SOURCE_STOPPED;enter(s,RADAR_SESSION_COPY,now);break;
    case RADAR_SESSION_COPY:
        if(d->completed_packets>r->stop_target || r->frames_verified>r->stop_target){failed(r,RADAR_SESSION_E_DATA,0);break;}
        if(d->completed_packets!=r->stop_target || r->frames_verified!=r->stop_target || r->copy_lease ||
           r->metadata_received!=r->stop_target || d->packet_queued) {
            if(now-s->phase_us>PHASE_TIMEOUT_US)failed(r,RADAR_SESSION_E_TIMEOUT,0);
            break;
        }
        if(d->partial_prefix){r->frames_residual=1U;failed(r,RADAR_SESSION_E_DATA,0);break;}
        rc=stop_drain_proof(r);if(rc){failed(r,rc,0);break;}
        r->stop_unused_bds=(u32)d->ring->HwCnt;
        if(r->stop_unused_bds) {
            /* A continuously replenished ring has unused preposted commands.
             * The board did not complete RS->Halted for this case. Do not
             * retry that failed path or assume a next TLAST is required by
             * PG021. Use the already qualified native graceful soft reset
             * once, AFTER the positive source/drain proof, not after a fault. */
            r->stop_by_reset=1U;
            (void)radar_console_printf(s->console,"[STREAM DRAIN PROOF] epoch=%u frames=%u bytes=%llu released_bds=%u unused_bds=%u; source ACK, boundary/HSI/CRC/metadata matched; RUN_TO_RESET\r\n",
                s->active_session,r->stop_target,(unsigned long long)d->completed_bytes,d->released_bds,r->stop_unused_bds);
            rc=radar_dma_reset_authorized(d);if(rc<0){failed(r,RADAR_SESSION_E_DMA,rc);break;}
            enter(s,RADAR_SESSION_RESET,now);break;
        }
        rc=radar_dma_stop_begin(d);if(rc<0){failed(r,RADAR_SESSION_E_DMA,rc);break;}
        enter(s,RADAR_SESSION_HALT,now);break;
    case RADAR_SESSION_HALT:
        if(d->state!=RADAR_DMA_HALTED)break;
        if(d->halt_result!=RADAR_DMA_PHASE_PASS || !radar_dma_is_quiescent(d)){failed(r,RADAR_SESSION_E_DMA,0);break;}
        rc=radar_dma_reset_begin(d);if(rc<0){failed(r,RADAR_SESSION_E_DMA,rc);break;}
        enter(s,RADAR_SESSION_RESET,now);break;
    case RADAR_SESSION_RESET:
        if(d->state!=RADAR_DMA_READY)break;
        if(d->reset_result!=RADAR_DMA_PHASE_PASS || !d->reset_confirmed || d->epoch!=s->admission_epoch+1U ||
           d->ring->FreeCnt!=(int)RADAR_RX_BD_COUNT || !radar_dma_is_quiescent(d)){
            failed(r,RADAR_SESSION_E_DMA,0);break;}
        rc=r->io.transport_end(r->io.context,s->active_session);if(rc){failed(r,RADAR_STREAM_E_METADATA,rc);break;}
        r->metadata_open=0U;
        rc=radar_sw2_capture_io(s->sw2,0U);if(!rc)rc=exec(s,RADAR_OP_RECOVER,0xC1EAU);
        if(rc){failed(r,RADAR_SESSION_E_RECOVERY,rc);break;}enter(s,RADAR_SESSION_RECOVER_AFTER,now);break;
    case RADAR_SESSION_RECOVER_AFTER:
        rc=recovered(s);if(rc==1){if(now-s->phase_us>100000U)failed(r,RADAR_SESSION_E_TIMEOUT,0);break;}
        if(rc){failed(r,RADAR_SESSION_E_RECOVERY,rc);break;}rc=radar_sw2_capture_release(s->sw2,0);
        if(rc){failed(r,RADAR_SESSION_E_OWNER,rc);break;}
        s->owns=0U;r->active=0U;r->error=(int)r->stop_reason;
        if(r->error)++s->captures_failed;else ++s->captures_passed;
        enter(s,RADAR_SESSION_IDLE,now);
        (void)radar_console_printf(s->console,"[STREAM STOP] epoch=%u verified=%u reason=%d path=%s unused_bds=%u; source ACK, packet-boundary drain, reset/Halted confirmed\r\n",
            (unsigned)s->active_session,(unsigned)r->frames_verified,r->error,
            r->stop_by_reset?"RUN_TO_RESET":"HALT_RESET",r->stop_unused_bds);break;
    default:failed(r,RADAR_SESSION_E_OWNER,0);break;
    }
}
