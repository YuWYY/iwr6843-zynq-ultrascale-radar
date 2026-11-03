/* SPDX-License-Identifier: MIT
 * Validated against the uploaded Vitis 2025.1 SDT AXI DMA BSP.
 * One thread owns this object. Drivers/PL initialization are not hot-attached.
 */
#include "radar_dma.h"
#include "radar_hw.h"
#include "radar_memory.h"
#include "xil_cache.h"
#include <stdlib.h>
#include <string.h>

#define DMA_SG_INCLUDED UINT32_C(0x00000008)
#define DMA_TRANSITION_US RADAR_DMA_TRANSITION_US
#define DMA_POLL_BOUND UINT32_C(1000000)
#define DMA_SLOT_NONE UINT32_C(0xffffffff)
#define DMA_DESC_ALIGNMENT XAXIDMA_BD_MINIMUM_ALIGNMENT
#define DMA_PACKET_MAX UINT32_C(8388600)

_Static_assert(RADAR_RX_BD_COUNT==128U, "Current pool/ring contract");
_Static_assert(RADAR_RX_POOL_BYTES/RADAR_RX_SLICE_BYTES==RADAR_RX_BD_COUNT,
               "One 64KiB payload slice per descriptor");
_Static_assert(RADAR_RX_SLICE_BYTES % RADAR_CACHE_LINE_BYTES == 0U,
               "No shared payload/metadata cache lines");

static XAxiDma_Bd *bd_at(const RadarDma *d,u32 slot)
{
    return (XAxiDma_Bd *)(d->bd_base+(UINTPTR)slot*d->ring->Separation);
}
static u32 slot_of(const RadarDma *d,const XAxiDma_Bd *p)
{
    UINTPTR addr=(UINTPTR)p,off;
    if (!d->ring || addr<d->bd_base || d->ring->Separation==0U) { return DMA_SLOT_NONE; }
    off=addr-d->bd_base;
    if (off % d->ring->Separation!=0U || off/d->ring->Separation>=RADAR_RX_BD_COUNT) {
        return DMA_SLOT_NONE;
    }
    return (u32)(off/d->ring->Separation);
}
static u32 next_slot(u32 i) { return (i+1U)%RADAR_RX_BD_COUNT; }
static UINTPTR payload(const RadarDma *d,u32 i)
{
    return d->rx_base+(UINTPTR)i*RADAR_RX_SLICE_BYTES;
}
static u32 status_word(XAxiDma_Bd *p)
{
    /* Actual completion memory is DMA-owned and can change independently.
     * BSP BdRead isn't volatile. Explicit volatile read + DSB before/after
     * harvesting avoids treating completion as ordinary cached C state. */
    return *(volatile u32 *)((UINTPTR)p+XAXIDMA_BD_STS_OFFSET);
}
static u32 clamp_us(u64 value)
{
    return value > UINT32_MAX ? UINT32_MAX : (u32)value;
}
static void read_bd_view(const RadarDma *d, u32 index, RadarDmaBdView *v)
{
    volatile u32 *p;
    if (!d->ring_created || index >= RADAR_RX_BD_COUNT) { return; }
    p=(volatile u32 *)bd_at(d,index);
    v->address=(u32)(UINTPTR)p; v->slot=index;
    v->next_lo=p[XAXIDMA_BD_NDESC_OFFSET/4U];
    v->next_hi=p[XAXIDMA_BD_NDESC_MSB_OFFSET/4U];
    v->buffer_lo=p[XAXIDMA_BD_BUFA_OFFSET/4U];
    v->buffer_hi=p[XAXIDMA_BD_BUFA_MSB_OFFSET/4U];
    v->control=p[XAXIDMA_BD_CTRL_LEN_OFFSET/4U];
    v->status=p[XAXIDMA_BD_STS_OFFSET/4U];
    v->owner=d->slot[index].owner; v->valid=1U;
}
static void capture_registers(const RadarDma *d,RadarDmaRegisters *r)
{
    (void)radar_dma_probe(r);
    if (d->ring_created) {
        r->free_count=(u32)d->ring->FreeCnt; r->pre_count=(u32)d->ring->PreCnt;
        r->hw_count=(u32)d->ring->HwCnt; r->post_count=(u32)d->ring->PostCnt;
        read_bd_view(d,0U,&r->bd[0]); read_bd_view(d,1U,&r->bd[1]);
        read_bd_view(d,slot_of(d,(XAxiDma_Bd *)(UINTPTR)r->current),&r->bd[2]);
        if (r->bd[2].valid) { r->bd_status=r->bd[2].status; }
    }
    radar_hw_barrier();
}
static void trace_event(RadarDma *d, RadarDmaEvent event, u32 written)
{
    RadarDmaTrace *t=&d->trace[d->trace_head];
    memset(t,0,sizeof(*t)); capture_registers(d,&t->registers);
    t->event=(u32)event; t->state=(u32)d->state; t->written_cr=written;
    t->elapsed_us=clamp_us(t->registers.sampled_us-d->transition_us);
    t->error=d->last_error;
    d->trace_head=(d->trace_head+1U)%RADAR_DMA_TRACE_DEPTH;
    if (d->trace_count<RADAR_DMA_TRACE_DEPTH) { ++d->trace_count; }
}
static int fail(RadarDma *d,int rc,int native,u32 bdstat)
{
    capture_registers(d,&d->failure);
    if (bdstat!=0U) { d->failure.bd_status=bdstat; }
    d->last_error=rc; d->driver_error=native; d->state=RADAR_DMA_FAULT;
    d->reset_confirmed=0U;
    trace_event(d,RADAR_DMA_EVT_ERROR,0U);
    /* No automatic reset or memory reuse. Caller owns source isolation. */
    return rc;
}
static int config_ok(const XAxiDma_Config *c)
{
    return c && c->BaseAddr==RADAR_DMA_BASE && c->HasSg==1 &&
           c->HasMm2S==0 && c->HasS2Mm==1 && c->S2MmNumChannels==1 &&
           c->S2MmDataWidth==64 && c->S2MmBurstSize==64 &&
           c->HasS2MmDRE==0 && c->HasStsCntrlStrm==0 &&
           c->MicroDmaMode==0 && c->AddrWidth==32 && c->SgLengthWidth==23;
}
static int check_memory(RadarDma *d)
{
    UINTPTR b=radar_memory_bd_base(),r=radar_memory_rx_base();
    u64 bend=(u64)b+RADAR_BD_ZONE_BYTES,rend=(u64)r+RADAR_RX_POOL_BYTES;
    u32 i;
    if (b==0U || r==0U || (b & (RADAR_MMU_BLOCK_BYTES-1U))!=0U ||
        (r & (RADAR_MMU_BLOCK_BYTES-1U))!=0U || bend!=(u64)r ||
        bend<=(u64)b || rend<=(u64)r || rend>RADAR_DDR_APP_LIMIT ||
        (u32)XAxiDma_BdRingMemCalc(DMA_DESC_ALIGNMENT,RADAR_RX_BD_COUNT)>RADAR_BD_ZONE_BYTES) {
        return RADAR_DMA_E_MEMORY;
    }
    if (radar_hw_check_translation(b,0x44U,1)!=0 ||
        radar_hw_check_translation((UINTPTR)bend-1U,0x44U,1)!=0) { return RADAR_DMA_E_MAPPING; }
    for (i=0;i<RADAR_RX_POOL_BYTES/RADAR_MMU_BLOCK_BYTES;++i) {
        if (radar_hw_check_translation(r+(UINTPTR)i*RADAR_MMU_BLOCK_BYTES,0xFFU,1)!=0) {
            return RADAR_DMA_E_MAPPING;
        }
    }
    d->bd_base=b; d->rx_base=r; return 0;
}
void radar_dma_construct(RadarDma *d)
{
    if (d) { memset(d,0,sizeof(*d)); d->state=RADAR_DMA_OFF; }
}
int radar_dma_probe(RadarDmaRegisters *r)
{
    UINTPTR base=(UINTPTR)RADAR_DMA_BASE+XAXIDMA_RX_OFFSET;
    if (!r) { return RADAR_DMA_E_ARGUMENT; }
    memset(r,0,sizeof(*r));
    if (radar_hw_check_translation((UINTPTR)RADAR_DMA_BASE,0x00U,1)!=0) {
        return RADAR_DMA_E_MAPPING;
    }
    radar_hw_barrier();
    r->control=XAxiDma_ReadReg(base,XAXIDMA_CR_OFFSET);
    r->status=XAxiDma_ReadReg(base,XAXIDMA_SR_OFFSET);
    r->current=XAxiDma_ReadReg(base,XAXIDMA_CDESC_OFFSET);
    r->tail=XAxiDma_ReadReg(base,XAXIDMA_TDESC_OFFSET);
    r->sampled_us=radar_hw_now_us();
    radar_hw_barrier();
    return (r->status & DMA_SG_INCLUDED)!=0U ? 0 : RADAR_DMA_E_HARDWARE;
}
int radar_dma_registers(RadarDma *d,RadarDmaRegisters *r)
{
    if (!d || !r) { return RADAR_DMA_E_ARGUMENT; }
    capture_registers(d,r);
    return (r->status & DMA_SG_INCLUDED)!=0U ? 0 : RADAR_DMA_E_HARDWARE;
}
int radar_dma_is_quiescent(RadarDma *d)
{
    RadarDmaRegisters r;
    if (!d || !d->configured || d->lease_active ||
        d->state==RADAR_DMA_LOCKED || d->state==RADAR_DMA_HALT_TIMEOUT) { return 0; }
    return radar_dma_probe(&r)==0 &&
        (r.control & (XAXIDMA_CR_RESET_MASK|XAXIDMA_CR_RUNSTOP_MASK))==0U &&
        (r.status & XAXIDMA_HALTED_MASK)!=0U &&
        (r.status & XAXIDMA_ERR_ALL_MASK)==0U;
}
static int rebuild_ring(RadarDma *d)
{
    XAxiDma_Bd blank;
    int rc;
    RadarDmaRegisters r;
    if (radar_dma_probe(&r)!=0 || (r.control & (XAXIDMA_CR_RESET_MASK|XAXIDMA_CR_RUNSTOP_MASK))!=0U ||
        (r.status & XAXIDMA_HALTED_MASK)==0U || (r.status & XAXIDMA_ERR_ALL_MASK)!=0U ||
        !d->reset_confirmed || d->lease_active) { return RADAR_DMA_E_STATE; }
    /* The supplied 2025.1 BdRingCreate allocates CyclicBd even for noncyclic
     * operation. Free ONLY its previous allocation AFTER confirmed full reset,
     * before calling Create again. No driver counters or queue heads are edited. */
    if (d->ring_created) {
        free(d->ring->CyclicBd); d->ring->CyclicBd=NULL; d->ring_created=0U;
    }
    rc=(int)XAxiDma_BdRingCreate(d->ring,d->bd_base,d->bd_base,
                                DMA_DESC_ALIGNMENT,(int)RADAR_RX_BD_COUNT);
    if (rc!=XST_SUCCESS) { return fail(d,RADAR_DMA_E_DRIVER,rc,0U); }
    d->ring_created=1U;
    if (!d->ring->CyclicBd) { return fail(d,RADAR_DMA_E_MEMORY,0,0U); }
    memset(&blank,0,sizeof(blank));
    rc=XAxiDma_BdRingClone(d->ring,&blank);
    if (rc!=XST_SUCCESS) { return fail(d,RADAR_DMA_E_DRIVER,rc,0U); }
    XAxiDma_BdRingIntDisable(d->ring,XAXIDMA_IRQ_ALL_MASK);
    rc=XAxiDma_BdRingSetCoalesce(d->ring,1U,0U);
    if (rc!=XST_SUCCESS) { return fail(d,RADAR_DMA_E_DRIVER,rc,0U); }
    memset(d->slot,0,sizeof(d->slot)); memset(d->packet,0,sizeof(d->packet));
    d->packet_head=0U; d->packet_tail=0U; d->packet_queued=0U;
    d->packet_bytes=0U;d->fragments_per_packet=0U;d->finite_packets=0U;d->session=0U;
    d->submitted_packets=0U;d->completed_packets=0U;d->released_packets=0U;
    d->submitted_bds=0U;d->completed_bds=0U;d->released_bds=0U;
    d->completed_bytes=0U;d->partial_prefix=0U;d->stop_was_incomplete=0U;
    if (d->epoch==UINT32_MAX) { d->state=RADAR_DMA_LOCKED;return RADAR_DMA_E_OWNER; }
    ++d->epoch; d->state=RADAR_DMA_READY;
    radar_hw_barrier();
    return radar_dma_ring_check(d);
}
int radar_dma_open(RadarDma *d)
{
    XAxiDma_Config *cfg;
    RadarDmaRegisters r;
    int rc;
    if (!d || d->state!=RADAR_DMA_OFF || d->configured) { return RADAR_DMA_E_STATE; }
    cfg=XAxiDma_LookupConfig((UINTPTR)RADAR_DMA_BASE);
    if (!config_ok(cfg)) { return RADAR_DMA_E_BSP; }
    rc=check_memory(d); if (rc!=0) { return rc; }
    if (radar_hw_counter_hz()==0U) { return RADAR_DMA_E_TIMER; }
    rc=radar_dma_probe(&r); if (rc!=0) { return rc; }
    /* Do not seize a running engine or erase pre-existing fault evidence. */
    if ((r.control & (XAXIDMA_CR_RUNSTOP_MASK|XAXIDMA_CR_RESET_MASK))!=0U ||
        (r.status & XAXIDMA_HALTED_MASK)==0U || (r.status & XAXIDMA_ERR_ALL_MASK)!=0U) {
        d->failure=r; return RADAR_DMA_E_OWNER;
    }
    rc=XAxiDma_CfgInitialize(&d->device,cfg);
    d->ring=XAxiDma_GetRxRing(&d->device);
    if (rc!=XST_SUCCESS) {
        d->last_error=RADAR_DMA_E_DRIVER;d->driver_error=rc;
        capture_registers(d,&d->failure);d->state=RADAR_DMA_LOCKED;
        return RADAR_DMA_E_DRIVER;
    }
    d->configured=1U;
    if (!XAxiDma_HasSg(&d->device) || d->ring->DataWidth!=8 ||
        d->ring->MaxTransferLen!=UINT32_C(0x7fffff) || d->ring->Addr_ext!=0) {
        return fail(d,RADAR_DMA_E_BSP,0,0U);
    }
    d->reset_confirmed=1U;++d->reset_count;
    d->transition_us=radar_hw_now_us();
    trace_event(d,RADAR_DMA_EVT_OPEN,0U);
    return rebuild_ring(d);
}
int radar_dma_plan(u32 bytes,u32 packets,u32 *bds,u32 *prime_packets)
{
    u64 per,count;
    if (!bds || !prime_packets || bytes<8U || bytes>DMA_PACKET_MAX || (bytes&7U)!=0U) {
        return RADAR_DMA_E_ARGUMENT;
    }
    per=((u64)bytes+RADAR_RX_SLICE_BYTES-1U)/RADAR_RX_SLICE_BYTES;
    count=packets==0U ? RADAR_RX_BD_COUNT/per : packets;
    if (count==0U || count*per>RADAR_RX_BD_COUNT) { return RADAR_DMA_E_SPACE; }
    *bds=(u32)(count*per);*prime_packets=(u32)count;return 0;
}
static int queue_packets(RadarDma *d,u32 packets)
{
    XAxiDma_Bd *first,*cur;
    u32 n,seq,left,slot,i,p;
    int rc;
    u64 total=(u64)packets*d->fragments_per_packet;
    if (packets==0U || total>RADAR_RX_BD_COUNT ||
        total>(u64)XAxiDma_BdRingGetFreeCnt(d->ring) ||
        d->submitted_packets>UINT32_MAX-packets ||
        (d->finite_packets && (u64)d->submitted_packets+packets>d->finite_packets)) {
        return RADAR_DMA_E_SPACE;
    }
    n=(u32)total;
    rc=XAxiDma_BdRingAlloc(d->ring,(int)n,&first);
    if (rc!=XST_SUCCESS) { return fail(d,RADAR_DMA_E_DRIVER,rc,0U); }
    /* Validate all mappings/ownership before mutating any descriptor. */
    cur=first;
    for (i=0;i<n;++i) {
        slot=slot_of(d,cur);
        if (slot==DMA_SLOT_NONE || d->slot[slot].owner!=RADAR_BUFFER_FREE) {
            (void)XAxiDma_BdRingUnAlloc(d->ring,(int)n,first);
            return fail(d,RADAR_DMA_E_OWNER,0,0U);
        }
        cur=(XAxiDma_Bd *)(UINTPTR)XAxiDma_BdRingNext(d->ring,cur);
    }
    cur=first;
    for (p=0U;p<packets;++p) {
        left=d->packet_bytes;seq=d->submitted_packets+p+1U;
        for (i=0U;i<d->fragments_per_packet;++i) {
            RadarDmaSlot *s;
            u32 length=left>RADAR_RX_SLICE_BYTES?RADAR_RX_SLICE_BYTES:left;
            slot=slot_of(d,cur);s=&d->slot[slot];
            XAxiDma_BdClear(cur); /* leaves next pointer + hardware info intact */
            rc=XAxiDma_BdSetBufAddr(cur,payload(d,slot));
            if (rc==XST_SUCCESS) { rc=XAxiDma_BdSetLength(cur,length,d->ring->MaxTransferLen); }
            if (rc!=XST_SUCCESS) { return fail(d,RADAR_DMA_E_DRIVER,rc,0U); }
            XAxiDma_BdSetCtrl(cur,0U);XAxiDma_BdSetId(cur,slot);
            s->capacity=length;s->actual=0U;s->status=0U;s->packet_sequence=seq;
            s->first=(u8)(i==0U);s->last=(u8)(i+1U==d->fragments_per_packet);
            /* Whole isolated slice is cleaned+invalidated before HW ownership.
             * Do NOT flush after HW writes, which could overwrite fresh data. */
            Xil_DCacheFlushRange(payload(d,slot),RADAR_RX_SLICE_BYTES);
            Xil_DCacheInvalidateRange(payload(d,slot),RADAR_RX_SLICE_BYTES);
            s->owner=RADAR_BUFFER_DMA;left-=length;
            cur=(XAxiDma_Bd *)(UINTPTR)XAxiDma_BdRingNext(d->ring,cur);
        }
    }
    radar_hw_barrier();
    rc=XAxiDma_BdRingToHw(d->ring,(int)n,first);
    if (rc!=XST_SUCCESS) { return fail(d,RADAR_DMA_E_DRIVER,rc,0U); }
    radar_hw_barrier();d->submitted_packets+=packets;d->submitted_bds+=n;
    return 0;
}
int radar_dma_prepare(RadarDma *d,u32 bytes,u32 packets,u32 session,u32 stall_timeout_us)
{
    u32 n,prime;
    int rc;
    if (!d || d->state!=RADAR_DMA_READY || !d->configured || !d->ring_created ||
        !d->ring || !d->reset_confirmed || !radar_dma_is_quiescent(d) || d->packet_queued ||
        d->lease_active || XAxiDma_BdRingGetFreeCnt(d->ring)!=(int)RADAR_RX_BD_COUNT) {
        return RADAR_DMA_E_STATE;
    }
    if (stall_timeout_us==0U) { return RADAR_DMA_E_ARGUMENT; }
    rc=radar_dma_plan(bytes,packets,&n,&prime);if (rc!=0) { return rc; }
    d->packet_bytes=bytes;d->fragments_per_packet=(bytes+RADAR_RX_SLICE_BYTES-1U)/RADAR_RX_SLICE_BYTES;
    d->finite_packets=packets;d->session=session;d->stall_timeout_us=stall_timeout_us;
    d->last_error=0;d->driver_error=0;memset(&d->failure,0,sizeof(d->failure));
    rc=queue_packets(d,prime);
    if (rc==0) { d->state=RADAR_DMA_STAGED; }
    return rc;
}
int radar_dma_prepare_window(RadarDma *d,u32 bytes,u32 packets,u32 session,u32 stall_timeout_us)
{
    u32 bds,prime;int rc;
    if(!d || d->state!=RADAR_DMA_READY || !d->configured || !d->ring_created ||
       !d->ring || !d->reset_confirmed || !radar_dma_is_quiescent(d) || d->packet_queued ||
       d->lease_active || XAxiDma_BdRingGetFreeCnt(d->ring)!=(int)RADAR_RX_BD_COUNT)
        return RADAR_DMA_E_STATE;
    if(!stall_timeout_us || !session)return RADAR_DMA_E_ARGUMENT;
    rc=radar_dma_plan(bytes,0U,&bds,&prime);if(rc)return rc;
    if(prime>18U)prime=18U;
    if(packets && packets<prime)prime=packets;
    d->packet_bytes=bytes;d->fragments_per_packet=(bytes+RADAR_RX_SLICE_BYTES-1U)/RADAR_RX_SLICE_BYTES;
    d->finite_packets=packets;d->session=session;d->stall_timeout_us=stall_timeout_us;
    d->last_error=0;d->driver_error=0;memset(&d->failure,0,sizeof(d->failure));
    rc=queue_packets(d,prime);if(!rc)d->state=RADAR_DMA_STAGED;return rc;
}
int radar_dma_start(RadarDma *d)
{
    int rc;
    if (!d || d->state!=RADAR_DMA_STAGED || d->ring->HwCnt==0) { return RADAR_DMA_E_STATE; }
    radar_hw_barrier();rc=XAxiDma_BdRingStart(d->ring);
    if (rc!=XST_SUCCESS) { return fail(d,RADAR_DMA_E_DRIVER,rc,0U); }
    radar_hw_barrier();d->state=RADAR_DMA_RUNNING;d->reset_confirmed=0U;
    d->started_us=radar_hw_now_us();d->last_progress_us=d->started_us;
    d->transition_us=d->started_us;d->transition_polls=0U;
    d->last_poll_us=d->started_us;
    d->halt_result=RADAR_DMA_PHASE_NOT_RUN;
    d->reset_result=RADAR_DMA_PHASE_NOT_RUN;
    d->halt_elapsed_us=0U;d->reset_elapsed_us=0U;d->max_poll_gap_us=0U;
    trace_event(d,RADAR_DMA_EVT_START,0U);
    return 0;
}
static int harvest(RadarDma *d,u64 now)
{
    XAxiDma_Bd *first,*cur;
    u32 prefix=0U,i,idx,packets=0U,firstslot=0U,parts=0U;
    u32 expected_seq=d->completed_packets+1U;
    u64 bytes=0U;
    int got;
    cur=d->ring->HwHead;
    radar_hw_barrier();
    /* FromHw withholds a partial packet. Still catch descriptor errors without
     * EOF and record partial progress; don't call FromHw with BdLimit=1. */
    for (i=0;i<(u32)d->ring->HwCnt;++i) {
        u32 st=status_word(cur);
        if ((st & XAXIDMA_BD_STS_ALL_ERR_MASK)!=0U) { return fail(d,RADAR_DMA_E_HARDWARE,0,st); }
        if ((st & XAXIDMA_BD_STS_COMPLETE_MASK)==0U) { break; }
        ++prefix;cur=(XAxiDma_Bd *)(UINTPTR)XAxiDma_BdRingNext(d->ring,cur);
    }
    if (prefix!=d->partial_prefix) { d->last_progress_us=now;d->partial_prefix=prefix; }
    radar_hw_barrier();
    got=XAxiDma_BdRingFromHw(d->ring,(int)RADAR_RX_BD_COUNT,&first);
    radar_hw_barrier();
    if (got==0) { return 0; }
    if (got<0 || !first || (u32)got>RADAR_RX_BD_COUNT) { return fail(d,RADAR_DMA_E_DRIVER,got,0U); }
    /* Verify the whole returned batch before exposing ANY new payload. */
    cur=first;
    for (i=0;i<(u32)got;++i) {
        RadarDmaSlot *s;
        u32 st,len;
        idx=slot_of(d,cur);if (idx==DMA_SLOT_NONE) { return fail(d,RADAR_DMA_E_DESCRIPTOR,0,0U); }
        s=&d->slot[idx];st=status_word(cur);len=st & d->ring->MaxTransferLen;
        if (s->owner!=RADAR_BUFFER_DMA || XAxiDma_BdGetId(cur)!=idx ||
            XAxiDma_BdGetBufAddr(cur)!=payload(d,idx) ||
            XAxiDma_BdGetLength(cur,d->ring->MaxTransferLen)!=s->capacity ||
            (st & XAXIDMA_BD_STS_COMPLETE_MASK)==0U ||
            (st & XAXIDMA_BD_STS_ALL_ERR_MASK)!=0U) {
            return fail(d,RADAR_DMA_E_DESCRIPTOR,0,st);
        }
        if (!len || len!=s->capacity || (len&7U)!=0U) { return fail(d,RADAR_DMA_E_LENGTH,0,st); }
        if (((st & XAXIDMA_BD_STS_RXSOF_MASK)!=0U)!=(s->first!=0U) ||
            ((st & XAXIDMA_BD_STS_RXEOF_MASK)!=0U)!=(s->last!=0U) ||
            s->packet_sequence!=expected_seq || (parts==0U && !s->first)) {
            return fail(d,RADAR_DMA_E_BOUNDARY,0,st);
        }
        bytes+=len;++parts;
        if (s->last) {
            if (bytes!=d->packet_bytes || parts!=d->fragments_per_packet) {
                return fail(d,RADAR_DMA_E_LENGTH,0,st);
            }
            ++packets;++expected_seq;bytes=0U;parts=0U;
        }
        cur=(XAxiDma_Bd *)(UINTPTR)XAxiDma_BdRingNext(d->ring,cur);
    }
    if (parts!=0U || d->packet_queued+packets>RADAR_RX_BD_COUNT ||
        (d->finite_packets && d->completed_packets+packets>d->finite_packets)) {
        return fail(d,RADAR_DMA_E_BOUNDARY,0,0U);
    }
    cur=first;parts=0U;
    for (i=0;i<(u32)got;++i) {
        RadarDmaSlot *s;
        idx=slot_of(d,cur);s=&d->slot[idx];
        if (parts==0U) { firstslot=idx; }
        s->status=status_word(cur);s->actual=s->status & d->ring->MaxTransferLen;
        Xil_DCacheInvalidateRange(payload(d,idx),RADAR_RX_SLICE_BYTES);
        s->owner=RADAR_BUFFER_READY;++parts;
        if (s->last) {
            RadarDmaPacket *p=&d->packet[d->packet_tail];
            p->epoch=d->epoch;p->sequence=s->packet_sequence;p->session=d->session;
            p->bytes=d->packet_bytes;p->first_slot=firstslot;p->slot_count=parts;
            p->observed_us=now;
            d->packet_tail=(d->packet_tail+1U)%RADAR_RX_BD_COUNT;++d->packet_queued;
            parts=0U;
        }
        cur=(XAxiDma_Bd *)(UINTPTR)XAxiDma_BdRingNext(d->ring,cur);
    }
    radar_hw_barrier();d->completed_packets+=packets;d->completed_bds+=(u32)got;
    d->completed_bytes+=(u64)packets*d->packet_bytes;
    d->partial_prefix=0U;d->last_progress_us=now;
    return (int)packets;
}
int radar_dma_poll(RadarDma *d,u64 now)
{
    RadarDmaRegisters r;
    int rc, expired, quiet;
    u32 elapsed;
    if (!d) { return RADAR_DMA_E_ARGUMENT; }
    if (d->state!=RADAR_DMA_RUNNING && d->state!=RADAR_DMA_HALTING &&
        d->state!=RADAR_DMA_RESETTING) { return 0; }
    if (d->last_poll_us && now>=d->last_poll_us) {
        u32 gap=clamp_us(now-d->last_poll_us);
        if (gap>d->max_poll_gap_us) { d->max_poll_gap_us=gap; }
    }
    d->last_poll_us=now;
    rc=radar_dma_probe(&r);
    if (rc!=0) {
        /* A failed probe while resetting cannot license another reset/reuse. */
        if (d->state==RADAR_DMA_RESETTING) {
            capture_registers(d,&d->failure); d->last_error=rc;
            d->state=RADAR_DMA_LOCKED; d->reset_result=RADAR_DMA_PHASE_ERROR;
            trace_event(d,RADAR_DMA_EVT_ERROR,0U); return rc;
        }
        return fail(d,rc,0,0U);
    }
    if (d->state==RADAR_DMA_RESETTING || d->state==RADAR_DMA_HALTING) {
        elapsed=clamp_us(now-d->transition_us);
        ++d->transition_polls;
        expired=d->transition_polls>=DMA_POLL_BOUND || elapsed>=DMA_TRANSITION_US;
        quiet=(r.control & (XAXIDMA_CR_RESET_MASK|XAXIDMA_CR_RUNSTOP_MASK))==0U &&
              (r.status & XAXIDMA_HALTED_MASK)!=0U;
        /* Physical completion FIRST. A late observation is a failed latency
         * test, not evidence that a demonstrably halted engine is still live. */
        if (quiet && d->state==RADAR_DMA_HALTING) {
            d->halt_elapsed_us=elapsed;
            if ((r.status & XAXIDMA_ERR_ALL_MASK)!=0U) {
                d->halt_result=RADAR_DMA_PHASE_ERROR;
                return fail(d,RADAR_DMA_E_HARDWARE,0,0U);
            }
            rc=XAxiDma_Pause(&d->device); /* driver RunState AFTER actual halt */
            if (rc!=XST_SUCCESS) {
                d->halt_result=RADAR_DMA_PHASE_ERROR;
                return fail(d,RADAR_DMA_E_DRIVER,rc,0U);
            }
            d->halt_result=expired ? RADAR_DMA_PHASE_LATE : RADAR_DMA_PHASE_PASS;
            if (expired) { d->last_error=RADAR_DMA_E_DEADLINE; }
            d->state=RADAR_DMA_HALTED;
            trace_event(d,RADAR_DMA_EVT_HALT_DONE,0U); return 0;
        }
        if (quiet && d->state==RADAR_DMA_RESETTING &&
            XAxiDma_ResetIsDone(&d->device) && (r.status & XAXIDMA_ERR_ALL_MASK)==0U) {
            d->reset_elapsed_us=elapsed;
            d->reset_result=expired ? RADAR_DMA_PHASE_LATE : RADAR_DMA_PHASE_PASS;
            if (expired) { d->last_error=RADAR_DMA_E_DEADLINE; }
            d->reset_confirmed=1U; ++d->reset_count;
            trace_event(d,RADAR_DMA_EVT_RESET_DONE,0U);
            return rebuild_ring(d);
        }
        if (expired) {
            capture_registers(d,&d->failure); d->reset_confirmed=0U;
            if (d->state==RADAR_DMA_HALTING) {
                d->halt_result=RADAR_DMA_PHASE_TIMEOUT; d->halt_elapsed_us=elapsed;
                d->last_error=RADAR_DMA_E_HALT_TIMEOUT;
                d->state=RADAR_DMA_HALT_TIMEOUT;
                trace_event(d,RADAR_DMA_EVT_HALT_TIMEOUT,0U);
            } else {
                d->reset_result=RADAR_DMA_PHASE_TIMEOUT; d->reset_elapsed_us=elapsed;
                d->last_error=RADAR_DMA_E_RESET_TIMEOUT; d->state=RADAR_DMA_LOCKED;
                trace_event(d,RADAR_DMA_EVT_RESET_TIMEOUT,0U);
            }
            return d->last_error;
        }
        return 0;
    }
    if ((r.status & XAXIDMA_ERR_ALL_MASK)!=0U ||
        (r.control & (XAXIDMA_CR_RESET_MASK|XAXIDMA_CR_CYCLIC_MASK))!=0U) {
        return fail(d,RADAR_DMA_E_HARDWARE,0,0U);
    }
    if ((r.control & XAXIDMA_CR_RUNSTOP_MASK)==0U) {
        return fail(d,RADAR_DMA_E_HARDWARE,0,0U);
    }
    if ((r.status & XAXIDMA_HALTED_MASK)!=0U) {
        if (now-d->started_us>=DMA_TRANSITION_US || ++d->transition_polls>=DMA_POLL_BOUND) {
            return fail(d,RADAR_DMA_E_RUN_TIMEOUT,0,0U);
        }
        return 0;
    }
    rc=harvest(d,now); if (rc<0) { return rc; }
    if (d->ring->HwCnt>0 && now-d->last_progress_us>=d->stall_timeout_us) {
        return fail(d,RADAR_DMA_E_PAYLOAD_TIMEOUT,0,0U);
    }
    return rc;
}
static int token_matches(const RadarDma *d,const RadarDmaPacket *p)
{
    const RadarDmaPacket *q;
    if (!p || !d->packet_queued) { return 0; }
    q=&d->packet[d->packet_head];
    return p->epoch==d->epoch && p->epoch==q->epoch && p->sequence==q->sequence &&
        p->session==q->session && p->bytes==q->bytes && p->first_slot==q->first_slot &&
        p->slot_count==q->slot_count;
}
int radar_dma_acquire(RadarDma *d,RadarDmaPacket *p)
{
    u32 idx,i;
    if (!d || !p || (d->state!=RADAR_DMA_RUNNING && d->state!=RADAR_DMA_HALTED)) {
        return RADAR_DMA_E_STATE;
    }
    if (d->lease_active) { return RADAR_DMA_E_OWNER; }
    if (!d->packet_queued) { return RADAR_DMA_PENDING; }
    *p=d->packet[d->packet_head];idx=p->first_slot;
    for (i=0;i<p->slot_count;++i) {
        if (d->slot[idx].owner!=RADAR_BUFFER_READY) { return fail(d,RADAR_DMA_E_OWNER,0,0U); }
        idx=next_slot(idx);
    }
    idx=p->first_slot;
    for (i=0;i<p->slot_count;++i) { d->slot[idx].owner=RADAR_BUFFER_CPU;idx=next_slot(idx); }
    radar_hw_barrier();d->lease_active=1U;return 0;
}
int radar_dma_fragment(RadarDma *d,const RadarDmaPacket *p,u32 fragment,RadarDmaFragment *out)
{
    u32 idx;
    if (!d || !out || !d->lease_active || !token_matches(d,p) || fragment>=p->slot_count ||
        (d->state!=RADAR_DMA_RUNNING && d->state!=RADAR_DMA_HALTED)) { return RADAR_DMA_E_OWNER; }
    idx=(p->first_slot+fragment)%RADAR_RX_BD_COUNT;
    if (d->slot[idx].owner!=RADAR_BUFFER_CPU) { return RADAR_DMA_E_OWNER; }
    out->address=payload(d,idx);out->bytes=d->slot[idx].actual;return 0;
}
int radar_dma_release(RadarDma *d,const RadarDmaPacket *p)
{
    u32 i,idx;
    int rc;
    if (!d || !d->lease_active || !token_matches(d,p)) { return RADAR_DMA_E_OWNER; }
    if (d->state!=RADAR_DMA_RUNNING && d->state!=RADAR_DMA_HALTED && d->state!=RADAR_DMA_FAULT) {
        return RADAR_DMA_E_STATE;
    }
    idx=p->first_slot;
    for (i=0;i<p->slot_count;++i) {
        if (d->slot[idx].owner!=RADAR_BUFFER_CPU) { return RADAR_DMA_E_OWNER; }
        idx=next_slot(idx);
    }
    radar_hw_barrier();
    rc=XAxiDma_BdRingFree(d->ring,(int)p->slot_count,bd_at(d,p->first_slot));
    if (rc!=XST_SUCCESS) { return fail(d,RADAR_DMA_E_DRIVER,rc,0U); }
    idx=p->first_slot;
    for (i=0;i<p->slot_count;++i) { d->slot[idx].owner=RADAR_BUFFER_FREE;idx=next_slot(idx); }
    d->packet_head=(d->packet_head+1U)%RADAR_RX_BD_COUNT;--d->packet_queued;d->lease_active=0U;
    ++d->released_packets;d->released_bds+=p->slot_count;return 0;
}
int radar_dma_refill(RadarDma *d,u32 packets)
{
    if (!d || d->state!=RADAR_DMA_RUNNING || !d->fragments_per_packet) { return RADAR_DMA_E_STATE; }
    return queue_packets(d,packets);
}
int radar_dma_stop_begin(RadarDma *d)
{
    u32 cr,written;
    if (!d || !d->configured || d->state==RADAR_DMA_LOCKED ||
        d->state==RADAR_DMA_HALT_TIMEOUT || d->state==RADAR_DMA_RESETTING ||
        d->lease_active) { return RADAR_DMA_E_STATE; }
    if (d->state==RADAR_DMA_HALTING) { return RADAR_DMA_PENDING; }
    if (d->state==RADAR_DMA_HALTED) { return 0; }
    if (d->state!=RADAR_DMA_RUNNING && d->state!=RADAR_DMA_STAGED &&
        d->state!=RADAR_DMA_FAULT && d->state!=RADAR_DMA_READY) { return RADAR_DMA_E_STATE; }
    if (d->last_error==0) { capture_registers(d,&d->failure); }
    d->stop_was_incomplete=(u8)(d->finite_packets==0U || d->completed_packets!=d->finite_packets);
    d->transition_us=radar_hw_now_us(); d->transition_polls=0U;
    if (d->state!=RADAR_DMA_RUNNING) { d->max_poll_gap_us=0U; }
    d->last_poll_us=d->transition_us;
    d->halt_elapsed_us=0U;
    d->halt_result=RADAR_DMA_PHASE_PENDING; ++d->halt_requests;
    cr=XAxiDma_ReadReg(d->ring->ChanBase,XAXIDMA_CR_OFFSET);
    written=cr & ~(XAXIDMA_CR_RUNSTOP_MASK|XAXIDMA_IRQ_ALL_MASK);
    XAxiDma_WriteReg(d->ring->ChanBase,XAXIDMA_CR_OFFSET,written);
    radar_hw_barrier(); d->state=RADAR_DMA_HALTING;
    trace_event(d,RADAR_DMA_EVT_HALT_REQUEST,written); return RADAR_DMA_PENDING;
}
static int reset_request(RadarDma *d,int explicitly_authorized)
{
    if (!d || !d->configured || d->state==RADAR_DMA_LOCKED || d->lease_active ||
        (d->state==RADAR_DMA_HALT_TIMEOUT && !explicitly_authorized)) {
        return RADAR_DMA_E_STATE;
    }
    if (d->state==RADAR_DMA_RESETTING) { return RADAR_DMA_PENDING; }
    if (d->last_error==0) { capture_registers(d,&d->failure); }
    /* Exclude minutes spent at SAFE/quarantine waiting for an explicit command.
     * In a live run, retain that run's active maximum; start() resets it. */
    if (d->state!=RADAR_DMA_RUNNING && d->state!=RADAR_DMA_HALTING) {
        d->max_poll_gap_us=0U;
    }
    d->reset_confirmed=0U; d->reset_result=RADAR_DMA_PHASE_PENDING;
    d->reset_elapsed_us=0U;
    d->transition_us=radar_hw_now_us(); d->transition_polls=0U; ++d->reset_requests;
    d->last_poll_us=d->transition_us;
    XAxiDma_Reset(&d->device); radar_hw_barrier(); d->state=RADAR_DMA_RESETTING;
    trace_event(d,RADAR_DMA_EVT_RESET_REQUEST,XAXIDMA_CR_RESET_MASK);
    return RADAR_DMA_PENDING;
}
int radar_dma_reset_begin(RadarDma *d) { return reset_request(d,0); }
int radar_dma_reset_authorized(RadarDma *d) { return reset_request(d,1); }
int radar_dma_ring_check(RadarDma *d)
{
    u32 i;
    RadarDmaRegisters r;
    int rc;
    if (!d || !d->ring_created || radar_dma_probe(&r)!=0 ||
        (r.status & XAXIDMA_HALTED_MASK)==0U || (r.control & XAXIDMA_CR_RUNSTOP_MASK)!=0U) {
        return RADAR_DMA_E_STATE;
    }
    rc=XAxiDma_BdRingCheck(d->ring);
    if (rc!=XST_SUCCESS) { return fail(d,RADAR_DMA_E_DRIVER,rc,0U); }
    for (i=0;i<RADAR_RX_BD_COUNT;++i) {
        if (XAxiDma_BdRead(bd_at(d,i),XAXIDMA_BD_NDESC_OFFSET)!=(u32)(UINTPTR)bd_at(d,next_slot(i)) ||
            XAxiDma_BdRead(bd_at(d,i),XAXIDMA_BD_NDESC_MSB_OFFSET)!=0U) {
            return fail(d,RADAR_DMA_E_DESCRIPTOR,0,0U);
        }
    }
    return 0;
}
const char *radar_dma_state_name(RadarDmaState s)
{
    switch(s) {
    case RADAR_DMA_OFF:return "OFF";case RADAR_DMA_READY:return "READY";
    case RADAR_DMA_STAGED:return "STAGED";case RADAR_DMA_RUNNING:return "RUNNING";
    case RADAR_DMA_HALTING:return "HALTING";case RADAR_DMA_HALTED:return "HALTED";
    case RADAR_DMA_RESETTING:return "RESETTING";case RADAR_DMA_FAULT:return "FAULT";
    case RADAR_DMA_HALT_TIMEOUT:return "HALT_TIMEOUT_QUARANTINED";
    case RADAR_DMA_LOCKED:return "RESET_LOCKED";default:return "UNKNOWN";
    }
}
const char *radar_dma_error_name(int rc)
{
    switch(rc) {
    case 0:return "OK";case 1:return "PENDING";
    case RADAR_DMA_E_ARGUMENT:return "INVALID_LENGTH_OR_ARGUMENT";
    case RADAR_DMA_E_STATE:return "STATE_REFUSED";case RADAR_DMA_E_BSP:return "BSP_MISMATCH";
    case RADAR_DMA_E_MAPPING:return "MEMORY_ATTRIBUTE_OR_TRANSLATION";
    case RADAR_DMA_E_HARDWARE:return "DMA_HARDWARE_ERROR";case RADAR_DMA_E_DRIVER:return "DRIVER_ERROR";
    case RADAR_DMA_E_MEMORY:return "MEMORY_LAYOUT_OR_ALLOCATION";case RADAR_DMA_E_SPACE:return "INSUFFICIENT_FREE_DESCRIPTORS";
    case RADAR_DMA_E_DESCRIPTOR:return "DESCRIPTOR_CORRUPTION";case RADAR_DMA_E_LENGTH:return "ACTUAL_LENGTH_MISMATCH";
    case RADAR_DMA_E_BOUNDARY:return "SOF_EOF_OR_SEQUENCE_MISMATCH";case RADAR_DMA_E_TIMEOUT:return "TIMEOUT_MEMORY_QUARANTINED";
    case RADAR_DMA_E_HALT_TIMEOUT:return "HALT_TIMEOUT_RESET_NOT_ATTEMPTED";
    case RADAR_DMA_E_RESET_TIMEOUT:return "RESET_TIMEOUT_MEMORY_LOCKED";
    case RADAR_DMA_E_DEADLINE:return "COMPLETION_OBSERVED_LATE";
    case RADAR_DMA_E_RUN_TIMEOUT:return "RUN_TIMEOUT";
    case RADAR_DMA_E_PAYLOAD_TIMEOUT:return "PAYLOAD_PROGRESS_TIMEOUT";
    case RADAR_DMA_E_OWNER:return "OWNERSHIP_REFUSED";case RADAR_DMA_E_TIMER:return "TIMER_INVALID";
    default:return "UNKNOWN";
    }
}

const char *radar_dma_phase_name(RadarDmaPhaseResult r)
{
    switch (r) {
    case RADAR_DMA_PHASE_NOT_RUN:return "NOT_RUN";
    case RADAR_DMA_PHASE_PENDING:return "PENDING";
    case RADAR_DMA_PHASE_PASS:return "PASS";
    case RADAR_DMA_PHASE_LATE:return "LATE_FAIL_BUT_QUIESCENT";
    case RADAR_DMA_PHASE_TIMEOUT:return "TIMEOUT";
    case RADAR_DMA_PHASE_ERROR:return "ERROR";
    default:return "UNKNOWN";
    }
}
