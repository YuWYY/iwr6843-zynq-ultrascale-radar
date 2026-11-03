/* SPDX-License-Identifier: MIT
 * One bounded job, explicit cache ownership, no retry or silent PS fallback.
 */
#include "radar_dsp.h"
#include <string.h>
#define RUN_US UINT64_C(5000000)
#define CANCEL_US UINT64_C(100000)
#define POLL_BOUND UINT32_C(10000000)
static uint32_t rd(RadarDspDriver *d,uint32_t o) { return d->io.read(d->io.context,o); }
static void wr(RadarDspDriver *d,uint32_t o,uint32_t v) { d->io.write(d->io.context,o,v); }
static int inside(uint32_t a,uint32_t n)
{ return !(a&63U) && !(n&63U) && a>=RADAR_DSP_WORK_BASE &&
         (uint64_t)a+n<=RADAR_DSP_WORK_END; }
static int hex_identity(const char *text,unsigned digits)
{
    unsigned i;
    for(i=0U;i<digits;++i)if(!((text[i]>='0'&&text[i]<='9')||(text[i]>='a'&&text[i]<='f')))return 0;
    return text[digits]==0;
}
static int layout(const RadarDspJob *j)
{
    const uint32_t a[]={j->input,j->range_tmp,j->cube_out,j->rd_out,j->spectrum_out,j->candidates_out,j->temporal_state};
    const uint32_t n[]={RADAR_DSP_COMPLEX_BYTES,RADAR_DSP_COMPLEX_BYTES,RADAR_DSP_COMPLEX_BYTES,
        RADAR_DSP_RD_BYTES,RADAR_DSP_SPECTRUM_BYTES,RADAR_DSP_CANDIDATES_BYTES,RADAR_DSP_TEMPORAL_BYTES};
    unsigned i,k;
    uint32_t base;
    if(j->task_slot>=RADAR_DSP_JOB_SLOTS)return 0;
    base=RADAR_DSP_WORK_BASE+j->task_slot*RADAR_DSP_JOB_SLOT_BYTES;
    if(j->input!=base || j->range_tmp!=base+0xc0000U || j->cube_out!=base+0x180000U ||
       j->rd_out!=base+0x240000U || j->spectrum_out!=base+0x250000U ||
       j->candidates_out!=base+0x250800U || j->temporal_state!=base+0x251000U)return 0;
    if(j->input_bytes!=RADAR_DSP_COMPLEX_BYTES) return 0;
    if(j->range_capacity!=n[1] || j->cube_capacity!=n[2] || j->rd_capacity!=n[3] ||
       j->spectrum_capacity!=n[4] || j->candidates_capacity!=n[5] || j->temporal_capacity!=n[6])return 0;
    for(i=0;i<7U;++i) {
        if(!inside(a[i],n[i])) return 0;
        /* Keep publication slots outside every job's mutable footprint. */
        if(a[i]+n[i]>RADAR_DSP_VIEW_SLOT0) return 0;
        for(k=0;k<i;++k) if(a[i]<(uint64_t)a[k]+n[k] && a[k]<(uint64_t)a[i]+n[i]) return 0;
    }
    return 1;
}
void radar_dsp_job_layout(RadarDspJob *j)
{
    j->task_slot=0U;j->task_lease=0U;
    j->input=RADAR_DSP_INPUT;j->range_tmp=RADAR_DSP_RANGE;j->cube_out=RADAR_DSP_CUBE;
    j->rd_out=RADAR_DSP_RD;j->spectrum_out=RADAR_DSP_SPECTRUM;j->candidates_out=RADAR_DSP_CANDIDATES;
    j->temporal_state=RADAR_DSP_TEMPORAL;j->input_bytes=RADAR_DSP_COMPLEX_BYTES;
    j->baseline_threshold_q16=RADAR_DSP_DETECTION_THRESHOLD_Q16;
    j->enhanced_threshold_q16=j->baseline_threshold_q16;
    j->hold_threshold_q16=RADAR_DSP_HOLD_THRESHOLD_Q16;
    j->numeric_format=RADAR_DSP_NUMERIC_Q2_22;j->parameter_revision=RADAR_DSP_PARAMETER_REVISION;
    j->range_capacity=j->cube_capacity=RADAR_DSP_COMPLEX_BYTES;j->rd_capacity=RADAR_DSP_RD_BYTES;
    j->spectrum_capacity=RADAR_DSP_SPECTRUM_BYTES;j->candidates_capacity=RADAR_DSP_CANDIDATES_BYTES;
    j->temporal_capacity=RADAR_DSP_TEMPORAL_BYTES;
}
int radar_dsp_job_layout_slot(RadarDspJob *j,uint32_t slot,uint32_t lease)
{
    uint32_t offset;
    if(!j || slot>=RADAR_DSP_JOB_SLOTS || !lease)return RADAR_DSP_E_ARGUMENT;
    radar_dsp_job_layout(j);offset=slot*RADAR_DSP_JOB_SLOT_BYTES;
    j->task_slot=slot;j->task_lease=lease;
    j->input+=offset;j->range_tmp+=offset;j->cube_out+=offset;j->rd_out+=offset;
    j->spectrum_out+=offset;j->candidates_out+=offset;j->temporal_state+=offset;
    return 0;
}
void radar_dsp_init(RadarDspDriver *d,const RadarDspIo *io)
{ memset(d,0,sizeof(*d));d->need_temporal_reset=1;if(io)d->io=*io; }
int radar_dsp_probe(RadarDspDriver *d)
{
    if(!d || !d->io.mapped || !d->io.read || !d->io.write || !d->io.now ||
       !d->io.flush || !d->io.invalidate || !d->io.barrier) return RADAR_DSP_E_UNAVAILABLE;
    if(radar_dsp_busy(d)) return RADAR_DSP_E_OWNER;
    if(rd(d,0)!=RADAR_DSP_MAGIC || rd(d,4)!=RADAR_DSP_ABI) { d->capable=0;d->need_temporal_reset=1;return RADAR_DSP_E_IDENTITY; }
    if(rd(d,12)&1U) return RADAR_DSP_E_OWNER;
    if(rd(d,0x10U)&UINT32_C(0x80000000)) {
        d->capable=1;d->recoverable=1;d->state=RADAR_DSP_LOCKED;d->need_temporal_reset=1;
        d->error=RADAR_DSP_E_LOCKED;return d->error;
    }
    d->capable=1;return 0;
}
static int submit(RadarDspDriver *d,const RadarDspJob *j,int diagnostic)
{
    uint32_t cfg[22];unsigned i;int rc;
    if(!d || !j || !j->job_id || !j->frame.frame_id || !j->frame.acquisition_id || !j->config_epoch ||
       (j->backend!=PL_BASELINE && j->backend!=PL_ENHANCED) ||
       !hex_identity(j->frame.cfg_sha256,64U) || !hex_identity(j->frame.profile_id,16U) ||
       j->numeric_format!=RADAR_DSP_NUMERIC_Q2_22 || j->parameter_revision!=RADAR_DSP_PARAMETER_REVISION)
        return RADAR_DSP_E_ARGUMENT;
    if(diagnostic) {
        if(j->backend!=PL_BASELINE || j->frame.bytes!=RADAR_DSP_COMPLEX_BYTES ||
           j->frame.flags!=(RADAR_FRAME_DIAGNOSTIC_PACKED|RADAR_FRAME_CRC_VALID) ||
           j->frame.raw!=(const uint8_t *)(uintptr_t)j->input || j->frame.timestamp_us)
            return RADAR_DSP_E_ARGUMENT;
    } else if((j->frame.flags&3U)!=3U || j->frame.bytes!=399360U ||
              (j->frame.flags&RADAR_FRAME_DIAGNOSTIC_PACKED)) return RADAR_DSP_E_ARGUMENT;
    if(d->state==RADAR_DSP_LOCKED) return RADAR_DSP_E_LOCKED;
    if(radar_dsp_busy(d)) return RADAR_DSP_E_OWNER;
    /* Survives temporal history clears and untrusted-time fallback. A new
     * job number/config epoch cannot turn a repeated source frame fresh. */
    if(d->submitted && (j->frame.acquisition_id<d->last_acquisition ||
       (j->frame.acquisition_id==d->last_acquisition && j->frame.frame_index<=d->last_frame_index)))
        return RADAR_DSP_E_IDENTITY;
    if(d->submitted && j->config_epoch==d->job.config_epoch &&
       (strcmp(j->frame.cfg_sha256,d->job.frame.cfg_sha256) || strcmp(j->frame.profile_id,d->job.frame.profile_id)))
        return RADAR_DSP_E_IDENTITY;
    if(!layout(j)) return RADAR_DSP_E_MEMORY;
    if(j->task_lease && j->task_lease<=d->slot_lease[j->task_slot])return RADAR_DSP_E_OWNER;
    if(j->frame.capture_epoch && (!j->task_lease || !j->frame.raw_lease ||
       j->frame.raw_slot>=16U || j->frame.transport_sequence!=j->frame.frame_index+1U ||
       j->frame.capture_epoch!=j->frame.acquisition_id))return RADAR_DSP_E_IDENTITY;
    rc=radar_dsp_probe(d);if(rc)return rc;
    if(d->submitted && j->config_epoch!=d->job.config_epoch)d->need_temporal_reset=1;
    d->job=*j;memset(&d->result,0,sizeof d->result);d->error=0;d->polls=0;
    cfg[0]=(uint32_t)j->backend;cfg[1]=j->job_id;cfg[2]=j->config_epoch;
    cfg[3]=j->input;cfg[4]=j->range_tmp;cfg[5]=j->cube_out;cfg[6]=j->rd_out;
    cfg[7]=j->spectrum_out;cfg[8]=j->candidates_out;cfg[9]=j->temporal_state;
    cfg[10]=j->input_bytes;cfg[11]=(uint32_t)j->frame.timestamp_us;
    cfg[12]=(uint32_t)(j->frame.timestamp_us>>32);cfg[13]=j->frame.frame_index;
    cfg[14]=j->frame.acquisition_id;cfg[15]=j->frame.flags;cfg[16]=j->alpha_q31;
    /* The frozen hardware ABI requires two input-validity bits even for
     * baseline arithmetic. This adapter certifies verified packed input to
     * that ABI; the software source remains DIAGNOSTIC, never HSI_VALID.
     * Diagnostic jobs cannot enter temporal enhancement or publication. */
    if(diagnostic)cfg[15]=RADAR_FRAME_DIAGNOSTIC_PACKED|3U;
    cfg[17]=j->temporal_control|((d->need_temporal_reset&&j->backend==PL_ENHANCED)?1U:0U);
    d->job.temporal_control=cfg[17];cfg[18]=j->baseline_threshold_q16;
    cfg[19]=j->enhanced_threshold_q16;cfg[20]=j->hold_threshold_q16;
    /* RTL checks old header time/identity before epoch/manual reset. A new
     * software time origin must not compare against that old header. Clear
     * only its complete cache line after source-identity and idle checks;
     * cells are initialized by the enhanced engine's explicit reset. */
    if(j->backend==PL_ENHANCED && (cfg[17]&1U)) {
        memset((void *)(uintptr_t)j->temporal_state,0,64U);
        d->io.flush(d->io.context,j->temporal_state,64U);
    }
    /* Clean possible cold-start writes before DMA; never clean stale output
     * cache after the engine has written new data. */
    d->io.flush(d->io.context,j->input,j->input_bytes);
    d->io.flush(d->io.context,j->range_tmp,RADAR_DSP_COMPLEX_BYTES);
    d->io.flush(d->io.context,j->cube_out,RADAR_DSP_COMPLEX_BYTES);
    d->io.flush(d->io.context,j->rd_out,RADAR_DSP_RD_BYTES);
    d->io.flush(d->io.context,j->spectrum_out,RADAR_DSP_SPECTRUM_BYTES);
    d->io.flush(d->io.context,j->candidates_out,RADAR_DSP_CANDIDATES_BYTES);
    d->io.flush(d->io.context,j->temporal_state,RADAR_DSP_TEMPORAL_BYTES);
    wr(d,8,4U);for(i=0;i<21U;++i)wr(d,0x14U+4U*i,cfg[i]);
    d->io.barrier(d->io.context);d->started_us=d->io.now(d->io.context);
    wr(d,8,1U);d->state=RADAR_DSP_RUNNING;
    d->last_acquisition=j->frame.acquisition_id;d->last_frame_index=j->frame.frame_index;
    if(j->task_lease)d->slot_lease[j->task_slot]=j->task_lease;
    ++d->submitted;return 0;
}
int radar_dsp_submit(RadarDspDriver *d,const RadarDspJob *j)
{ return submit(d,j,0); }
int radar_dsp_submit_diagnostic(RadarDspDriver *d,const RadarDspJob *j)
{ return submit(d,j,1); }
int radar_dsp_busy(const RadarDspDriver *d)
{ return d && (d->state==RADAR_DSP_RUNNING || d->state==RADAR_DSP_CANCELING || d->state==RADAR_DSP_LOCKED || d->state==RADAR_DSP_RECOVERING); }
int radar_dsp_recover(RadarDspDriver *d)
{
    if(!d || !d->capable || !d->io.mapped)return RADAR_DSP_E_UNAVAILABLE;
    if(d->state!=RADAR_DSP_LOCKED || !d->recoverable)return RADAR_DSP_E_OWNER;
    if((rd(d,12)&1U) || !(rd(d,0x10U)&UINT32_C(0x80000000)))return RADAR_DSP_E_OWNER;
    wr(d,8,8U);d->io.barrier(d->io.context);d->cancel_us=d->io.now(d->io.context);
    d->state=RADAR_DSP_RECOVERING;d->recoverable=0;d->need_temporal_reset=1;return 0;
}
int radar_dsp_cancel(RadarDspDriver *d)
{
    if(!d)return RADAR_DSP_E_ARGUMENT;
    if(d->state==RADAR_DSP_LOCKED)return RADAR_DSP_E_LOCKED;
    if(d->state==RADAR_DSP_CANCELING)return 0;
    if(d->state!=RADAR_DSP_RUNNING)return 0;
    wr(d,8,2U);d->cancel_us=d->io.now(d->io.context);d->state=RADAR_DSP_CANCELING;
    d->need_temporal_reset=1;
    if(!d->error)d->error=RADAR_DSP_E_CANCELED;
    ++d->canceled;return 0;
}
int radar_dsp_poll(RadarDspDriver *d)
{
    uint32_t st;uint64_t now;
    if(!d)return RADAR_DSP_E_ARGUMENT;
    if(d->state==RADAR_DSP_LOCKED)return RADAR_DSP_E_LOCKED;
    if(d->state==RADAR_DSP_COMPLETE)return RADAR_DSP_DONE;
    if(d->state==RADAR_DSP_FAILED)return d->error;
    if(!radar_dsp_busy(d))return RADAR_DSP_E_OWNER;
    now=d->io.now(d->io.context);st=rd(d,12);
    if(d->state==RADAR_DSP_RECOVERING) {
        if(!(st&1U)) {
            if(rd(d,0x10U) || rd(d,0)!=RADAR_DSP_MAGIC || rd(d,4)!=RADAR_DSP_ABI) {
                d->state=RADAR_DSP_LOCKED;d->error=RADAR_DSP_E_LOCKED;return d->error;
            }
            d->io.invalidate(d->io.context,d->job.temporal_state?d->job.temporal_state:RADAR_DSP_TEMPORAL,RADAR_DSP_TEMPORAL_BYTES);
            d->io.barrier(d->io.context);d->state=RADAR_DSP_IDLE;d->error=0;return RADAR_DSP_DONE;
        }
        if(now<d->cancel_us || now-d->cancel_us>=CANCEL_US) {
            d->state=RADAR_DSP_LOCKED;d->error=RADAR_DSP_E_LOCKED;return d->error;
        }
        return RADAR_DSP_PENDING;
    }
    if(!(st&1U) && (st&6U)) {
        const RadarDspJob *j=&d->job;RadarDspResult *r=&d->result;
        const int reset_before=d->need_temporal_reset;
        d->need_temporal_reset=1;
        d->io.barrier(d->io.context);
        r->error=rd(d,0x10U);
        if(r->error&UINT32_C(0x80000000)) {
            d->recoverable=1;d->state=RADAR_DSP_LOCKED;d->error=RADAR_DSP_E_LOCKED;return d->error;
        }
        if(d->state==RADAR_DSP_CANCELING) { d->state=RADAR_DSP_FAILED;return d->error; }
        if((st&4U) || r->error) { d->error=RADAR_DSP_E_HARDWARE;d->state=RADAR_DSP_FAILED;return d->error; }
        if(rd(d,0x18U)!=j->job_id || rd(d,0x1cU)!=j->config_epoch) {
            d->error=RADAR_DSP_E_IDENTITY;d->state=RADAR_DSP_FAILED;return d->error;
        }
        r->backend=j->backend;r->job_id=j->job_id;r->config_epoch=j->config_epoch;
        r->frame_id=j->frame.frame_id;r->acquisition_id=j->frame.acquisition_id;
        r->frame_index=j->frame.frame_index;r->timestamp_us=j->frame.timestamp_us;
        memcpy(r->cfg_sha256,j->frame.cfg_sha256,sizeof r->cfg_sha256);
        memcpy(r->profile_id,j->frame.profile_id,sizeof r->profile_id);
        r->numeric_format=j->numeric_format;r->parameter_revision=j->parameter_revision;r->input_bytes=j->input_bytes;
        r->cycles=(uint64_t)rd(d,0x68U)|((uint64_t)rd(d,0x6cU)<<32);
        r->candidate_count=rd(d,0x70U);r->overflow_count=rd(d,0x74U);
        r->vector_count=rd(d,0x78U);r->flags=rd(d,0x7cU);
        if(r->candidate_count>32U || r->overflow_count || r->vector_count!=3456U || (r->flags&RADAR_DSP_RESULT_CELL_ORDER_ERROR)) {
            d->error=RADAR_DSP_E_RESULT;d->state=RADAR_DSP_FAILED;return d->error;
        }
        d->io.invalidate(d->io.context,j->cube_out,RADAR_DSP_COMPLEX_BYTES);
        d->io.invalidate(d->io.context,j->rd_out,RADAR_DSP_RD_BYTES);
        d->io.invalidate(d->io.context,j->spectrum_out,RADAR_DSP_SPECTRUM_BYTES);
        d->io.invalidate(d->io.context,j->candidates_out,RADAR_DSP_CANDIDATES_BYTES);
        d->io.invalidate(d->io.context,j->temporal_state,RADAR_DSP_TEMPORAL_BYTES);
        if(j->backend==PL_ENHANCED && !(r->flags&(RADAR_DSP_RESULT_INVALID_INPUT|
            RADAR_DSP_RESULT_IDENTITY_RESET|RADAR_DSP_RESULT_TIME_UNTRUSTED)))d->need_temporal_reset=0;
        if(j->backend==PL_BASELINE)d->need_temporal_reset=reset_before;
        d->io.barrier(d->io.context);r->valid=1;d->state=RADAR_DSP_COMPLETE;++d->completed;return RADAR_DSP_DONE;
    }
    if(d->state==RADAR_DSP_RUNNING && (now<d->started_us || now-d->started_us>=RUN_US || ++d->polls>=POLL_BOUND)) {
        d->error=RADAR_DSP_E_TIMEOUT;(void)radar_dsp_cancel(d);
    } else if(d->state==RADAR_DSP_CANCELING && (now<d->cancel_us || now-d->cancel_us>=CANCEL_US)) {
        d->state=RADAR_DSP_LOCKED;d->error=RADAR_DSP_E_LOCKED;return d->error;
    }
    return RADAR_DSP_PENDING;
}
const RadarDspResult *radar_dsp_result(const RadarDspDriver *d)
{ return d && d->state==RADAR_DSP_COMPLETE && d->result.valid ? &d->result : NULL; }
int radar_dsp_metadata_clear(RadarDspDriver *d)
{
    /* Independent passive observer: a current DSP job must not prevent the
     * next capture from clearing its own timestamp queue before ARM. */
    if(!d || !d->io.mapped || !d->io.read || !d->io.write || !d->io.barrier)return RADAR_DSP_E_UNAVAILABLE;
    if(rd(d,0)!=RADAR_DSP_MAGIC || rd(d,4)!=RADAR_DSP_ABI)return RADAR_DSP_E_IDENTITY;
    d->capable=1;
    wr(d,0x11cU,2U);d->io.barrier(d->io.context);
    return (rd(d,0x100U)&UINT32_C(0xa000007f))?RADAR_DSP_E_RESULT:0;
}
int radar_dsp_metadata_pop(RadarDspDriver *d,RadarTransportTime *t)
{
    uint32_t status,after;
    if(!d || !t || !d->capable)return RADAR_DSP_E_UNAVAILABLE;
    memset(t,0,sizeof *t);status=rd(d,0x100U);if(!(status&127U))return 0;
    t->sequence=rd(d,0x108U);
    t->first_tick=(uint64_t)rd(d,0x10cU)|((uint64_t)rd(d,0x110U)<<32);
    t->last_tick=(uint64_t)rd(d,0x114U)|((uint64_t)rd(d,0x118U)<<32);
    t->bytes=rd(d,0x120U);t->hz=rd(d,0x124U);after=rd(d,0x100U);
    t->status=status|after;
    t->valid=!(t->status&UINT32_C(0xa0000000)) && t->sequence && t->hz &&
        t->last_tick>=t->first_tick && t->sequence==rd(d,0x108U);
    d->io.barrier(d->io.context);wr(d,0x11cU,1U);return 1;
}
