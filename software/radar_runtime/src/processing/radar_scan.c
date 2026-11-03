/* SPDX-License-Identifier: MIT
 * Cooperative finite-frame scheduler. It reuses every native capture gate;
 * no automatic reset/recovery/retry. Live mode explicitly repeats complete
 * finite frames, including native source stop, teardown and requalification.
 */
#include "radar_scan.h"
#include "radar_crc32.h"
#include "radar_hw.h"
#include "radar_memory.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <limits.h>

#define SCAN_PRESENT_US UINT64_C(3000000)
#define SCAN_PHASE_US UINT64_C(30000000)
#define SCAN_LOG_ROOM 2048U
#define SAY(s,...) radar_console_printf((s)->session->console,__VA_ARGS__)

uint32_t radar_scan_alpha_q31(uint64_t delta_us)
{
    double scaled=-expm1(-(double)delta_us/2000000.0)*2147483648.0;
    uint32_t integer;double fraction;
    if(scaled<=0.0)return 0U;
    if(scaled>=2147483648.0)return UINT32_C(0x80000000);
    integer=(uint32_t)scaled;fraction=scaled-(double)integer;
    if(fraction>0.5 || (fraction==0.5 && (integer&1U)))++integer;
    return integer;
}

static void visible(RadarScan *s,const char *status)
{
    s->status=status;
    if(++s->revision==0U) ++s->revision;
}

static void phase(RadarScan *s,RadarScanState state,const char *status)
{
    s->state=state;
    s->phase_us=radar_hw_now_us();
    if(s->live_mode) {
        /* Only admission/new-frame events need a new display revision in
         * live mode. Control setup never redraws an unchanged range plot. */
        if(state==RADAR_SCAN_WAIT_INITIAL_PRESENT || state==RADAR_SCAN_WAIT_FRAME_PRESENT)
            visible(s,s->stop_requested ? "LIVE STOPPING" : "LIVE");
        return;
    }
    visible(s,status);
}

static int profile_ok(const RadarSession *c)
{
    const RadarCaptureProfile *p=c->profile;
    return p && p->profile_id && p->cfg_sha256 &&
        radar_capture_profile_supported(p) &&
        !radar_capture_profile_validate(p) &&
        p->qualification==RADAR_CAPTURE_FINITE_FRAME_TEST &&
        p->packet_bytes==RADAR_RANGE_INPUT_BYTES &&
        (p->packet_count==1U || p->packet_count==8U || p->packet_count==16U) &&
        p->format_flags==8U && p->skip_pairs==0U && c->confirmed;
}

static int hardware_busy(const RadarSession *c)
{
    return c->owns || c->exporting || c->sw3->job!=SW3_IDLE ||
        c->sw2->job!=SW2_JOB_NONE || c->sw2->state==SW2_CONNECT_WAIT ||
        radar_cli_busy(&c->sw2->cli) || c->control->pending;
}

static int retained_ok(const RadarSession *c)
{
    const RadarCaptureResult *r=&c->result;
    return profile_ok(c) && !hardware_busy(c) &&
        c->state==RADAR_SESSION_DONE && r->valid &&
        r->session && r->session==c->active_session &&
        r->bytes==RADAR_RANGE_INPUT_BYTES*c->profile->packet_count &&
        r->packet_bytes==RADAR_RANGE_INPUT_BYTES && r->packet_count==c->profile->packet_count &&
        r->format==8U && !r->skip_pairs &&
        r->startup_mode==c->profile->startup_mode &&
        r->snapshot.first_sys_tick &&
        r->snapshot.complete_sys_tick>=r->snapshot.first_sys_tick;
}
static const RadarCaptureResult *consumer_data(const RadarScan *s)
{
    return s->session->raw_store_owner ? &s->consumer_result : &s->session->result;
}
static int consumer_ok(const RadarScan *s)
{
    if(!s->session->raw_store_owner)return retained_ok(s->session);
    return s->pipeline && profile_ok(s->session) && s->consumer_result.valid &&
        s->session->raw_store_owner==s->consumer_result.session &&
        s->consumer_result.bytes==RADAR_RANGE_INPUT_BYTES*s->session->profile->packet_count;
}

static void timing_begin(RadarScan *s,const RadarCaptureResult *batch,const RadarFrameRef *frame)
{
    RadarTimingRecord *t=radar_timing_begin(&s->timing,frame->frame_id);
    const RadarTransportTime *transport;
    if(!t)return;
    t->acquisition_id=frame->acquisition_id;t->frame_index=frame->frame_index;
    t->backend=(uint32_t)s->backend;t->crc32=frame->crc32;t->frame_flags=frame->flags;
    t->queue_depth=s->frames.count;t->queue_high_water=s->frames.high_water;
    t->observed_ps_us=frame->observed_us;t->batch_begin_ps_us=batch->begin_us;
    t->armed_ps_us=batch->armed_us;t->halt_ps_us=batch->complete_us;
    t->copy_begin_ps_us=batch->copy_begin_us;t->copy_complete_ps_us=batch->copy_complete_us;
    t->ready_ps_us=batch->ready_us;t->processing_begin_ps_us=radar_hw_now_us();
    if(frame->frame_index>=16U)return;
    transport=&batch->frame_transport[frame->frame_index];
    t->transport_sequence=transport->sequence;t->transport_bytes=transport->bytes;
    t->transport_hz=transport->hz;t->transport_status=transport->status;
    t->transport_first_tick=transport->first_tick;t->transport_last_tick=transport->last_tick;
    if(transport->valid)t->stages|=RADAR_TIMING_TRANSPORT;
}

static void timing_fail(RadarScan *s,int error)
{
    RadarTimingRecord *t=radar_timing_current(&s->timing);
    if(!t || t->frame_id!=s->processing_session ||
       (t->stages&(RADAR_TIMING_FAILED|RADAR_TIMING_PUBLISHED)))return;
    t->error=error;t->stages|=RADAR_TIMING_FAILED;++s->timing.failed;
}

static int read_rate(RadarScan *s)
{
    u32 hz=0U;
    if(hardware_busy(s->session) ||
       radar_control_read32(s->session->control,RADAR_PAGE_SYS_HZ,&hz) ||
       !hz || hz>1000000000U || (s->have_origin && s->sys_hz!=hz)) {
        SAY(s,"[SCOPE BLOCK] SYS clock read/rate changed; preserve history, inspect clock, then scope clear.\r\n");
        return -1;
    }
    s->sys_hz=hz;
    return 0;
}

/* Resume only a known stopped session with intact software/physical admission.
 * Fresh DMA qualification and CLI version query still precede full preflight. */
static int resume_safe(const RadarScan *s)
{
    const RadarSession *c=s->session;
    int known_completed=s->count && s->have_origin && s->last_session &&
        s->history[s->count-1U].valid &&
        s->history[s->count-1U].session==s->last_session &&
        c->active_session==s->last_acquisition &&
        c->next_session>s->last_acquisition && c->source==RADAR_SOURCE_STOPPED;
    /* An explicit stop before the very first display ACK has no completed
     * frame identity. These counters distinguish it from any failed session. */
    int pristine=!s->count && !s->have_origin && !s->last_session &&
        c->next_session==1U && !c->active_session &&
        !c->captures_passed && !c->captures_failed && !c->error &&
        c->state==RADAR_SESSION_IDLE && c->source==RADAR_SOURCE_UNKNOWN;
    return profile_ok(c) && !hardware_busy(c) &&
        (known_completed || pristine) && c->sw2->state==SW2_SAFE &&
        c->sw2->wiring_confirmed && c->sw2->check10_passes &&
        c->sw2->reset3_passes && !c->sw2->fatal_control &&
        !c->sw2->cli.needs_reset && !c->sw3->check_error &&
        !c->control->desynced && c->control->abi_verified;
}

static void finish(RadarScan *s,const char *status)
{
    s->active=0U;
    s->state=RADAR_SCAN_IDLE;
    s->batch_mode=0U;
    s->live_mode=0U;
    s->resume_empty=0U;
    visible(s,status);
    SAY(s,"[SCOPE %s] frames=%u/%u history=%u retained_session=%u\r\n",
        status,(unsigned)s->batch_done,(unsigned)s->batch_total,
        (unsigned)s->count,(unsigned)s->session->result.session);
}

/* Used only between captures, after any short native transaction has ended.
 * The completed finite session already stopped RF; this only removes UART
 * output enables. Never substitute this for a failed capture's recovery. */
static void stop_between(RadarScan *s)
{
    RadarSession *c=s->session;
    if(!c->owns && c->sw3->job==SW3_IDLE && c->sw2->job==SW2_JOB_NONE &&
       c->sw2->state!=SW2_CONNECT_WAIT && !radar_cli_busy(&c->sw2->cli)) {
        if(c->sw2->state!=SW2_SAFE)
            radar_sw2_command(c->sw2,"safe");
        finish(s,c->sw2->state==SW2_SAFE ?
               (s->live_mode ? "LIVE STOPPED" : "STOPPED") : "SAFE FAILED");
    }
}

static void fail(RadarScan *s,const char *reason)
{
    /* A valid transport teardown alone cannot renew reuse after a parser,
     * per-frame CRC, identity or processing failure. No hardware access. */
    radar_session_invalidate_config(s->session);
    timing_fail(s,s->dsp.error?s->dsp.error:-1);
    s->dsp.need_temporal_reset=1;
    if(s->producer_state!=RADAR_SCAN_IDLE) {
        if(s->session->owns)radar_session_cancel(s->session);
        else {
            if(s->session->sw3->job!=SW3_IDLE)(void)radar_sw3_command(s->session->sw3,"safe");
            radar_sw2_command(s->session->sw2,"safe");
        }
        s->producer_state=RADAR_SCAN_IDLE;
    }
    if(radar_dsp_busy(&s->dsp)) { (void)radar_dsp_cancel(&s->dsp);s->cancel_pending=1U; }
    else {
        if(s->frames.lease)(void)radar_frame_queue_release(&s->frames,s->frames.lease);
        (void)radar_frame_queue_clear(&s->frames);
    }
    radar_range_cancel(&s->processor);
    radar_spatial_cancel(&s->spatial_processor);
    SAY(s,"[SCOPE FAILED] %s; no retry/reset/recovery. Native state and raw result retained.\r\n",reason);
    finish(s,"FAILED");
}

/* Spatial rejection must not destroy otherwise valid range/CRC evidence. */
static int spatial_sink(void *context,uint32_t chirp,uint32_t rx,
                        const float *re,const float *im)
{
    RadarScan *s=(RadarScan *)context;
    if(!s->spatial_error) {
        int rc=radar_spatial_accept_fft(&s->spatial_processor,chirp,rx,re,im);
        if(rc) {
            s->spatial_error=rc<0 ? rc : RADAR_SPATIAL_E_STATE;
            radar_spatial_cancel(&s->spatial_processor);
        }
    }
    return 0;
}

static int begin_processing(RadarScan *s)
{
    const RadarCaptureResult *r=consumer_data(s);
    const RadarFrameRef *ref;
    u64 delta,seconds,us;unsigned i;int rc;
    if(!consumer_ok(s) || !s->sys_hz || radar_dsp_busy(&s->dsp)) {
        fail(s,"retained batch ownership or DSP lease");return -1;
    }
    if(s->pipeline && !s->session->raw_store_owner) {
        if(radar_session_lease_result(s->session,&s->consumer_result)) {
            fail(s,"raw consumer lease refused");return -1;
        }
        r=&s->consumer_result;
    }
    if(!s->frames.count) {
        if(r->session<=s->last_acquisition ||
           (s->have_origin && r->snapshot.first_sys_tick<=s->last_tick)) {
            fail(s,"retained batch identity or timestamp");return -1;
        }
        delta=s->have_origin?r->snapshot.first_sys_tick-s->origin_tick:0U;
        seconds=delta/s->sys_hz;
        if(seconds>UINT64_MAX/UINT64_C(1000000)) { fail(s,"timestamp overflow");return -1; }
        us=seconds*UINT64_C(1000000)+(delta%s->sys_hz)*UINT64_C(1000000)/s->sys_hz;
        if(!s->origin_observed_us)s->origin_observed_us=r->frame_observed_us[0]?r->frame_observed_us[0]:r->complete_us;
        s->source_crc=r->crc32;
        if(s->next_frame_id<r->session)s->next_frame_id=r->session;
        for(i=0U;i<r->packet_count;++i) {
            RadarFrameRef f={0};u64 observed=r->frame_observed_us[i]?r->frame_observed_us[i]:r->complete_us;
            if(!s->next_frame_id || observed<s->origin_observed_us) { fail(s,"frame identity/time exhausted");return -1; }
            f.raw=(const u8 *)radar_memory_capture_base()+(size_t)i*RADAR_RANGE_INPUT_BYTES;
            f.bytes=RADAR_RANGE_INPUT_BYTES;f.frame_id=s->next_frame_id++;
            f.acquisition_id=r->session;f.frame_index=i;f.observed_us=observed;
            memcpy(f.cfg_sha256,s->session->profile->cfg_sha256,sizeof f.cfg_sha256);
            memcpy(f.profile_id,s->session->profile->profile_id,sizeof f.profile_id);
            f.crc32=r->packet_count==1U?r->crc32:r->frame_crc32[i];
            f.flags=RADAR_FRAME_CRC_VALID;
            f.timestamp_us=r->packet_count==1U?us:observed-s->origin_observed_us;
            if(r->packet_count==1U)f.flags|=RADAR_FRAME_TIME_TRUSTED;
            if(r->frame_transport[i].valid) {
                const RadarTransportTime *t=&r->frame_transport[i];
                if(!s->transport_hz) {
                    s->transport_origin_tick=t->first_tick;s->transport_hz=t->hz;
                    s->transport_origin_us=f.timestamp_us;
                }
                if(t->hz==s->transport_hz && t->first_tick>=s->transport_origin_tick) {
                    u64 ticks=t->first_tick-s->transport_origin_tick;
                    f.timestamp_us=s->transport_origin_us+(ticks/t->hz)*UINT64_C(1000000)+(ticks%t->hz)*UINT64_C(1000000)/t->hz;
                    f.flags|=RADAR_FRAME_TIME_TRUSTED|RADAR_FRAME_TIME_TRANSPORT;
                } else f.flags&=~RADAR_FRAME_TIME_TRUSTED;
            }
            if(radar_frame_queue_publish(&s->frames,&f)) { fail(s,"frame queue overflow/order");return -1; }
        }
        if(s->pipeline && s->batch_mode && !s->stop_requested &&
           (s->live_mode || s->batch_done+s->frames.count<s->batch_total)) {
            s->producer_state=RADAR_SCAN_BEGIN_QUALIFY;s->producer_phase_us=radar_hw_now_us();
        }
    }
    ref=radar_frame_queue_acquire(&s->frames);
    if(!ref) { fail(s,"frame consumer lease");return -1; }
    s->processing_frame=*ref;
    s->processing_session=ref->frame_id;
    timing_begin(s,r,ref);
    rc=radar_range_begin(&s->processor,ref->raw,ref->bytes,ref->frame_id,ref->timestamp_us);
    if(rc<0) { SAY(s,"[SCOPE DSP] %s\r\n",radar_range_error_name(rc));fail(s,"HSI parse rejected");return -1; }
    s->processing_frame.flags|=RADAR_FRAME_HSI_VALID;
    radar_timing_current(&s->timing)->frame_flags=s->processing_frame.flags;
    s->spatial_error=0;s->spatial_calls=s->spatial_slices=0U;s->spatial_max_slice_us=0U;
    rc=radar_spatial_begin(&s->spatial_processor,ref->frame_id,ref->timestamp_us,NULL);
    if(rc<0)s->spatial_error=rc;
    s->capture_observed_us=ref->observed_us;s->processing_tick=r->snapshot.first_sys_tick;
    s->processing_session=ref->frame_id;s->processing_acquisition=r->session;
    s->expected_crc=ref->crc32;s->processing_crc=UINT32_MAX;s->processing_record=0U;
    s->dsp_phase=s->pl_import_row=0U;
    phase(s,RADAR_SCAN_PROCESS,"PROCESSING");return 0;
}

static void start_native(RadarScan *s)
{
    int rc;
    if(!profile_ok(s->session) || radar_session_preflight(s->session)) {
        radar_session_print_preflight(s->session);
        fail(s,"native capture preflight");
        return;
    }
    rc=radar_session_start(s->session);
    if(rc) {
        fail(s,"native capture start");
        return;
    }
    phase(s,RADAR_SCAN_CAPTURE,"CAPTURING");
}

static void process_one(RadarScan *s)
{
    const u64 slice_start=radar_hw_now_us();
    const RadarCaptureResult *r=consumer_data(s);
    const RadarRangeFrame *f;
    const u8 *raw=s->processing_frame.raw;
    RadarTimingRecord *timing=radar_timing_current(&s->timing);
    int rc;
    if(!consumer_ok(s) || r->session!=s->processing_acquisition ||
       r->crc32!=s->source_crc || r->snapshot.first_sys_tick!=s->processing_tick ||
       strcmp(s->processing_frame.cfg_sha256,s->session->profile->cfg_sha256) ||
       strcmp(s->processing_frame.profile_id,s->session->profile->profile_id) ||
       s->processing_record>RADAR_RANGE_CHIRPS) {
        fail(s,"retained frame changed during processing");
        return;
    }
    if(s->processing_record<RADAR_RANGE_CHIRPS) {
        if(timing && !s->processing_record)timing->front_begin_ps_us=radar_hw_now_us();
        s->processing_crc=radar_crc32_update(s->processing_crc,
            raw+s->processing_record*RADAR_RANGE_RECORD_BYTES,RADAR_RANGE_RECORD_BYTES);
        ++s->processing_record;
        rc=s->backend==PS_REFERENCE?radar_range_step(&s->processor):
            radar_range_pack_step(&s->processor,(int32_t *)(uintptr_t)RADAR_DSP_INPUT,196608U);
        if(rc<0) {
            SAY(s,"[SCOPE DSP] %s\r\n",radar_range_error_name(rc));
            fail(s,"range processing rejected");
            return;
        }
        if(rc!=RADAR_RANGE_DONE) return;
    }
    if((s->processing_crc^UINT32_MAX)!=s->expected_crc) { fail(s,"retained per-frame CRC changed");return; }
    if(timing && !(timing->stages&RADAR_TIMING_FRONT_DONE)) {
        timing->front_end_ps_us=radar_hw_now_us();timing->stages|=RADAR_TIMING_FRONT_DONE;
    }
    if(s->backend!=PS_REFERENCE) {
        if(s->dsp_phase==0U) {
            RadarDspJob job={0};
            job.frame=s->processing_frame;
            if(s->processor.frame.clipped_components)job.frame.flags|=RADAR_FRAME_CLIPPED;
            if(s->dsp.submitted && (strcmp(job.frame.cfg_sha256,s->dsp.job.frame.cfg_sha256) ||
               strcmp(job.frame.profile_id,s->dsp.job.frame.profile_id))) {
                if(s->dsp_config_epoch==UINT32_MAX) { fail(s,"DSP configuration epoch exhausted");return; }
                ++s->dsp_config_epoch;s->have_previous_dsp_time=0U;
            }
            job.backend=s->backend;job.job_id=++s->dsp_job_id;job.config_epoch=s->dsp_config_epoch;
            if(!job.job_id) { fail(s,"DSP job sequence exhausted");return; }
            radar_dsp_job_layout(&job);
            /* Temporal coefficients are metadata, never a claim that sparse
             * batch-completion observations are trusted acquisition times. */
            if(s->have_previous_dsp_time && job.frame.timestamp_us>s->previous_dsp_time) {
                job.alpha_q31=radar_scan_alpha_q31(job.frame.timestamp_us-s->previous_dsp_time);
            }
            if((job.frame.flags&7U)==7U && !(job.frame.flags&24U)) {
                s->previous_dsp_time=job.frame.timestamp_us;s->have_previous_dsp_time=1U;
            } else s->have_previous_dsp_time=0U;
            if(timing) {
                timing->job_id=job.job_id;timing->config_epoch=job.config_epoch;
                timing->frame_flags=job.frame.flags;timing->submit_begin_ps_us=radar_hw_now_us();
            }
            rc=radar_dsp_submit(&s->dsp,&job);
            if(timing)timing->submit_end_ps_us=radar_hw_now_us();
            if(rc) { SAY(s,"[DSP FAIL] submit=%d backend=%u\r\n",rc,(unsigned)s->backend);fail(s,"PL submit refused; no fallback");return; }
            if(timing)timing->stages|=RADAR_TIMING_SUBMITTED;
            SAY(s,"[DSP JOB] job=%u frame=%u acquisition=%u index=%u epoch=%u format=Q2.22 parameter=%s profile=%s cfg=%s\r\n",
                job.job_id,job.frame.frame_id,job.frame.acquisition_id,job.frame.frame_index,job.config_epoch,
                RADAR_DSP_PARAMETER_VERSION,job.frame.profile_id,job.frame.cfg_sha256);
            s->dsp_phase=1U;return;
        }
        if(s->dsp_phase==1U) {
            const RadarDspCandidateHeader *header;
            rc=radar_dsp_poll(&s->dsp);if(rc==RADAR_DSP_PENDING)return;
            if(rc!=RADAR_DSP_DONE) { fail(s,"PL execution failed; no fallback");return; }
            if(timing) {
                timing->done_ps_us=radar_hw_now_us();timing->pl_cycles=s->dsp.result.cycles;
                timing->stages|=RADAR_TIMING_DONE;
            }
            if(!radar_dsp_result(&s->dsp) || s->dsp.result.frame_id!=s->processing_session ||
               strcmp(s->dsp.result.cfg_sha256,s->processing_frame.cfg_sha256) ||
               strcmp(s->dsp.result.profile_id,s->processing_frame.profile_id) ||
               s->dsp.result.numeric_format!=RADAR_DSP_NUMERIC_Q2_22 ||
               s->dsp.result.parameter_revision!=RADAR_DSP_PARAMETER_REVISION) { fail(s,"PL result identity");return; }
            rc=radar_range_import_power(&s->processor,(const uint64_t *)(uintptr_t)RADAR_DSP_SPECTRUM);
            if(rc!=RADAR_RANGE_DONE) { fail(s,"PL spectrum import");return; }
            header=(const RadarDspCandidateHeader *)(uintptr_t)RADAR_DSP_CANDIDATES;
            if(header->count!=s->dsp.result.candidate_count) { fail(s,"PL candidate header mismatch");return; }
            if(s->backend==PL_ENHANCED &&
               (s->dsp.result.flags&(RADAR_DSP_RESULT_INVALID_INPUT|RADAR_DSP_RESULT_IDENTITY_RESET))) {
                s->spatial_error=RADAR_SPATIAL_E_TEMPORAL_INPUT;s->dsp_phase=3U;
            } else {
            rc=radar_spatial_import_pl_begin(&s->spatial_processor,header,(const RadarDspCandidate *)(header+1),s->backend);
            if(rc<0) { fail(s,"PL candidate import");return; }
            s->dsp_phase=2U;
            }
        }
        if(s->dsp_phase==2U) {
            u32 row=s->pl_import_row;
            rc=radar_spatial_import_pl_row(&s->spatial_processor,row,
                (const int32_t *)(uintptr_t)RADAR_DSP_CUBE+(size_t)row*12U*32U*2U,
                (const uint64_t *)(uintptr_t)RADAR_DSP_RD+(size_t)row*32U);
            if(rc<0) { fail(s,"PL complex cube import");return; }
            ++s->pl_import_row;if(rc!=RADAR_SPATIAL_DONE)return;
            s->dsp_phase=3U;
        }
    }
    f=radar_range_result(&s->processor);
    if(s->processing_record!=RADAR_RANGE_CHIRPS ||
       (s->processing_crc^UINT32_MAX)!=s->expected_crc || !f ||
       !f->valid || f->session!=s->processing_session) {
        fail(s,"retained CRC or processed-frame identity");
        return;
    }
    /* The retained raw frame and its profile remain owned by this foreground
     * processing phase. Recheck the complete existing guard once per bounded
     * slice, not once per tiny32-point FFT. No ISR mutates these identities.
     * Return to the same console/key/native-service loop between slices. */
    ++s->spatial_slices;
    rc=s->spatial_error ? s->spatial_error : RADAR_SPATIAL_BUSY;
    if(!s->spatial_error) {
        unsigned calls=0U;
        while(calls<RADAR_SCAN_SPATIAL_MAX_STEPS &&
              radar_hw_now_us()-slice_start<RADAR_SCAN_SPATIAL_SLICE_US) {
            rc=radar_spatial_step(&s->spatial_processor);
            ++calls; ++s->spatial_calls;
            if(rc!=RADAR_SPATIAL_BUSY) break;
        }
    }
    {
        u64 elapsed=radar_hw_now_us()-slice_start;
        if(elapsed>s->spatial_max_slice_us) s->spatial_max_slice_us=elapsed;
    }
    if(rc==RADAR_SPATIAL_BUSY) return;
    if(rc<0) {
        /* Valid range acquisition survives an explicitly invalid spatial
         * frame. A bad raw identity or CRC above never takes this path. */
        memset(&s->spatial_frame,0,sizeof(s->spatial_frame));
        s->spatial_frame.session=f->session;
        s->spatial_frame.timestamp_us=f->timestamp_us;
        s->spatial_frame.error=rc;
        strcpy(s->spatial_frame.calibration_id,"UNCALIBRATED");
        radar_spatial_cancel(&s->spatial_processor);
    } else {
        const RadarSpatialFrame *sp=radar_spatial_result(&s->spatial_processor);
        if(!sp || !sp->valid || sp->session!=f->session || sp->timestamp_us!=f->timestamp_us) {
            fail(s,"spatial/range publication identity");
            return;
        }
        s->spatial_frame=*sp;
    }
    if(timing) {
        timing->post_done_ps_us=radar_hw_now_us();timing->stages|=RADAR_TIMING_POST_DONE;
        timing->error=s->spatial_frame.error;
    }
    if(s->count==RADAR_SCAN_HISTORY) {
        memmove(s->history,s->history+1U,
                (RADAR_SCAN_HISTORY-1U)*sizeof(s->history[0]));
        --s->count;
    }
    s->history[s->count]=*f;
    s->history[s->count].time_untrusted=!(s->processing_frame.flags&RADAR_FRAME_TIME_TRUSTED);
    ++s->count;
    if(!s->have_origin) {
        s->origin_tick=s->processing_tick;
        s->have_origin=1U;
    }
    s->last_tick=s->processing_tick;
    s->last_session=s->processing_session;
    s->last_acquisition=s->processing_acquisition;
    s->published_session=s->processing_session;
    s->published_observed_us=s->capture_observed_us;
    {
        RadarFrameRef sample=s->processing_frame;
        if(s->processor.frame.clipped_components)sample.flags|=RADAR_FRAME_CLIPPED;
        radar_freshness_capture(&s->published_freshness,&sample,s->backend,
            s->backend==PS_REFERENCE?0U:s->dsp.result.flags,s->dsp_config_epoch,
            f->valid && s->spatial_frame.error==0);
        s->published_freshness.spatial_valid=s->spatial_frame.valid;
        s->published_freshness.detection_count=s->spatial_frame.count;
    }
    if(s->batch_mode && s->batch_done!=UINT32_MAX) ++s->batch_done;
    SAY(s,"[SCOPE FRAME] session=%u frame=%u/%u history=%u timestamp_us=%llu crc=%08x peak_bin=%u peak_mdbfs=%d clipped=%u processing_us=%llu\r\n",
        (unsigned)f->session,(unsigned)s->batch_done,(unsigned)s->batch_total,
        (unsigned)s->count,(unsigned long long)f->timestamp_us,
        (unsigned)s->expected_crc,(unsigned)f->peak_bin,(int)(f->peak_dbfs*1000.0f),
        (unsigned)f->clipped_components,(unsigned long long)(radar_hw_now_us()-s->phase_us));
    SAY(s,"[SCOPE ANTENNAS] session=%u hsi=%u fft=%u tx_records=%u,%u,%u tx_mdbfs=%d,%d,%d\r\n",
        (unsigned)f->session,(unsigned)f->hsi_records,(unsigned)f->fft_count,
        (unsigned)f->tx_records[0],(unsigned)f->tx_records[1],(unsigned)f->tx_records[2],
        (int)(f->tx_power_dbfs[0]*1000.0f),(int)(f->tx_power_dbfs[1]*1000.0f),
        (int)(f->tx_power_dbfs[2]*1000.0f));
    SAY(s,"[SCOPE SPATIAL] session=%u valid=%u error=%d count=%u xyz=%u candidates=%u truncated=%u calibrated=%u\r\n",
        (unsigned)s->spatial_frame.session,(unsigned)s->spatial_frame.valid,
        (int)s->spatial_frame.error,(unsigned)s->spatial_frame.count,
        (unsigned)s->spatial_frame.xyz_count,(unsigned)s->spatial_frame.candidate_count,
        (unsigned)s->spatial_frame.truncated_count,(unsigned)s->spatial_frame.calibrated);
    SAY(s,"[SCOPE DSP BUDGET] session=%u calls=%u slices=%u max_slice_us=%llu budget_us=500 step_limit=256\r\n",
        (unsigned)f->session,(unsigned)s->spatial_calls,(unsigned)s->spatial_slices,
        (unsigned long long)s->spatial_max_slice_us);
    SAY(s,"[FRAME ID] frame=%u acquisition=%u index=%u backend=%u timestamp_source=%s flags=%u\r\n",
        (unsigned)s->processing_session,(unsigned)s->processing_acquisition,
        (unsigned)s->processing_frame.frame_index,(unsigned)s->backend,
        (s->processing_frame.flags&RADAR_FRAME_TIME_TRANSPORT)?"TRANSPORT_SYS_FIRST_BEAT":
        ((s->processing_frame.flags&RADAR_FRAME_TIME_TRUSTED)?"PL_BATCH_FIRST":"DMA_COMPLETION_OBSERVED"),
        (unsigned)s->processing_frame.flags);
    if(s->backend==PL_ENHANCED)SAY(s,"[TEMPORAL] frame=%u flags=%08x time=%s detector=%s; fallback is not fresh temporal evidence\r\n",
        (unsigned)s->processing_session,(unsigned)s->dsp.result.flags,
        (s->dsp.result.flags&RADAR_DSP_RESULT_TIME_UNTRUSTED)?"UNAVAILABLE":"TRUSTED_TRANSPORT_OR_SNAPSHOT",
        (s->dsp.result.flags&RADAR_DSP_RESULT_BASELINE_FALLBACK)?"BASELINE_FALLBACK":"ENHANCED_OR_INVALID");
    if(s->backend!=PS_REFERENCE)SAY(s,"[DSP RESULT] job=%u frame=%u epoch=%u format=%u parameter_revision=%u cycles=%llu vectors=%u cfg=%s\r\n",
        s->dsp.result.job_id,s->dsp.result.frame_id,s->dsp.result.config_epoch,s->dsp.result.numeric_format,
        s->dsp.result.parameter_revision,(unsigned long long)s->dsp.result.cycles,s->dsp.result.vector_count,s->dsp.result.cfg_sha256);
    if(radar_frame_queue_release(&s->frames,s->processing_session)) { fail(s,"frame lease release");return; }
    if(s->pipeline && !s->frames.count &&
       radar_session_release_raw(s->session,s->processing_acquisition)) {
        fail(s,"raw consumer release refused");return;
    }
    if(s->pipeline && !s->frames.count && s->producer_state==RADAR_SCAN_IDLE &&
       !s->session->owns && !s->session->result.valid &&
       s->session->active_session==s->processing_acquisition) {
        s->session->result=s->consumer_result;s->session->state=RADAR_SESSION_DONE;
    }
    phase(s,RADAR_SCAN_WAIT_FRAME_PRESENT,"FRAME READY");
    if(timing) {
        timing->published_revision=s->revision;timing->published_ps_us=radar_hw_now_us();
        timing->stages|=RADAR_TIMING_PUBLISHED;++s->timing.published;
    }
}

/* One bounded producer step, independent of the consumer's FFT/import state.
 * Original session owns UART/GPIO/DMA. It halts RX before waiting for RAW. */
static void producer_poll(RadarScan *s)
{
    RadarSession *c=s->session;
    if(s->producer_state==RADAR_SCAN_IDLE)return;
    if(radar_hw_now_us()-s->producer_phase_us>=SCAN_PHASE_US) {
        fail(s,"prefetch producer watchdog; RX/raw retained");return;
    }
    if(s->producer_state==RADAR_SCAN_CAPTURE) {
        if(c->state==RADAR_SESSION_FAILED) { fail(s,"prefetch native capture failed");return; }
        if(!c->owns) {
            if(c->state!=RADAR_SESSION_DONE || !c->result.valid) { fail(s,"prefetch missing completed result");return; }
            s->producer_state=RADAR_SCAN_IDLE;
        }
        return;
    }
    if(c->sw3->job!=SW3_IDLE || c->sw2->job!=SW2_JOB_NONE ||
       c->sw2->state==SW2_CONNECT_WAIT || radar_cli_busy(&c->sw2->cli))return;
    if(s->stop_requested) {
        radar_sw2_command(c->sw2,"safe");s->producer_state=RADAR_SCAN_IDLE;return;
    }
    if(radar_console_free(c->console)<SCAN_LOG_ROOM)return;
    switch(s->producer_state) {
    case RADAR_SCAN_BEGIN_QUALIFY:
        if(c->owns || c->result.valid || c->state!=RADAR_SESSION_IDLE) { fail(s,"prefetch native result ownership");return; }
        (void)radar_sw3_command(c->sw3,"dma qualify");
        if(c->sw3->job==SW3_IDLE) { fail(s,"prefetch qualification refused");return; }
        s->producer_state=RADAR_SCAN_QUALIFY;break;
    case RADAR_SCAN_QUALIFY:
        if(c->sw3->check_error || !radar_sw3_capture_ready(c->sw3)) { fail(s,"prefetch qualification failed");return; }
        radar_sw2_command(c->sw2,"connect");
        if(c->sw2->state!=SW2_CONNECT_WAIT) { fail(s,"prefetch connect refused");return; }
        s->producer_state=RADAR_SCAN_CONNECT;break;
    case RADAR_SCAN_CONNECT:
        if(c->sw2->state!=SW2_ONLINE) { fail(s,"prefetch connect failed");return; }
        radar_sw2_command(c->sw2,"query");
        if(c->sw2->job!=SW2_JOB_QUERY) { fail(s,"prefetch query refused");return; }
        s->producer_state=RADAR_SCAN_QUERY;break;
    case RADAR_SCAN_QUERY:
        if(c->sw2->state!=SW2_ONLINE || !c->sw2->cli_verified || !c->sw2->cli.version_valid ||
           c->sw2->cli.version_epoch!=c->sw2->cli.reset_epoch ||
           !radar_capture_profile_version_matches(c->profile,c->sw2->cli.last_version) || radar_session_start(c)) {
            fail(s,"prefetch native admission failed");return;
        }
        ++s->prefetched_batches;s->producer_state=RADAR_SCAN_CAPTURE;break;
    default:fail(s,"prefetch state");return;
    }
    s->producer_phase_us=radar_hw_now_us();
}

void radar_scan_init(RadarScan *s,RadarSession *session)
{
    memset(s,0,sizeof(*s));
    s->session=session;
    s->status="READY";
    s->revision=1U;s->next_frame_id=1U;s->dsp_config_epoch=1U;
    radar_dsp_platform_init(&s->dsp);radar_frame_queue_init(&s->frames);
    radar_range_init(&s->processor);
    radar_spatial_init(&s->spatial_processor);
    (void)radar_range_set_fft_sink(&s->processor,spatial_sink,s);
}

int radar_scan_busy(const RadarScan *s) { return s && (s->active || radar_dsp_busy(&s->dsp)); }

void radar_scan_presented(RadarScan *s,u32 revision)
{
    if(s && revision && revision<=s->revision && revision>s->presented_revision) s->presented_revision=revision;
}

void radar_scan_display_ack(RadarScan *s,const RadarTimingAck *ack)
{
    if(s)radar_timing_ack(&s->timing,ack);
}

void radar_scan_poll(RadarScan *s)
{
    RadarSession *c;
    u64 elapsed;
    if(!s)return;
    if(s->cancel_pending) {
        int canceled=radar_dsp_poll(&s->dsp);
        if(canceled==RADAR_DSP_PENDING || s->dsp.state==RADAR_DSP_LOCKED)return;
        if(s->dsp.state==RADAR_DSP_IDLE) {
            if(s->dsp_config_epoch==UINT32_MAX) { s->dsp.state=RADAR_DSP_LOCKED;return; }
            ++s->dsp_config_epoch;s->previous_dsp_time=0U;s->have_previous_dsp_time=0U;
        }
        s->cancel_pending=0U;
        if(s->frames.lease)(void)radar_frame_queue_release(&s->frames,s->frames.lease);
        (void)radar_frame_queue_clear(&s->frames);
    }
    if(!s->active)return;
    producer_poll(s);if(!s->active)return;
    c=s->session;
    elapsed=radar_hw_now_us()-s->phase_us;
    if(s->state==RADAR_SCAN_WAIT_INITIAL_PRESENT ||
       s->state==RADAR_SCAN_WAIT_FRAME_PRESENT) {
        if(s->stop_requested && !(s->pipeline && s->frames.count)) { stop_between(s); return; }
        if(!s->display_decoupled && s->presented_revision!=s->revision) {
            if(elapsed>=SCAN_PRESENT_US)
                fail(s,"display acknowledgement timeout: no further hardware command");
            return;
        }
        if(s->state==RADAR_SCAN_WAIT_INITIAL_PRESENT) {
            start_native(s);
        } else if(s->frames.count) {
            (void)begin_processing(s);
        } else if(s->pipeline && (s->producer_state!=RADAR_SCAN_IDLE || c->owns)) {
            phase(s,RADAR_SCAN_CAPTURE,"WAIT PREFETCH");
        } else if(s->pipeline && c->result.valid && c->result.session>s->last_acquisition) {
            (void)begin_processing(s);
        } else if(!s->batch_mode || (!s->live_mode && s->batch_done>=s->batch_total)) {
            finish(s,s->batch_mode ? "COMPLETE" : "RETAINED FRAME");
        } else {
            phase(s,RADAR_SCAN_BEGIN_QUALIFY,"QUALIFY DMA");
        }
        return;
    }
    if(elapsed>=SCAN_PHASE_US) {
        /* Native services have shorter watchdogs. This final bound is a
         * cancellation, never a reset or an automatic retry. */
        if(c->owns) radar_session_cancel(c);
        else {
            if(c->sw3->job!=SW3_IDLE) (void)radar_sw3_command(c->sw3,"safe");
            radar_sw2_command(c->sw2,"safe");
        }
        fail(s,"phase watchdog canceled activity");
        return;
    }
    switch(s->state) {
    case RADAR_SCAN_CAPTURE:
        if(c->state==RADAR_SESSION_FAILED) { fail(s,"native capture failed"); break; }
        if(c->owns) break;
        if(s->pipeline && s->producer_state!=RADAR_SCAN_IDLE)break;
        if(s->pipeline && s->stop_requested) { stop_between(s);break; }
        if(c->state!=RADAR_SESSION_DONE || !c->result.valid) {
            fail(s,"capture ended without a valid DONE result"); break;
        }
        (void)begin_processing(s);
        break;
    case RADAR_SCAN_PROCESS:
        process_one(s);
        break;
    case RADAR_SCAN_BEGIN_QUALIFY:
        if(s->stop_requested) { stop_between(s); break; }
        if(radar_console_free(c->console)<SCAN_LOG_ROOM) break;
        if(s->resume_empty) {
            if(c->result.valid || c->state!=RADAR_SESSION_IDLE || !resume_safe(s)) {
                fail(s,"stopped empty-result resume identity changed"); break;
            }
            s->resume_empty=0U;
        } else {
            if(!retained_ok(c) || c->result.session!=s->last_acquisition) {
                fail(s,"retained result changed before requalification"); break;
            }
            (void)radar_session_command(c,"capture clear");
        }
        if(c->result.valid || c->state!=RADAR_SESSION_IDLE || c->owns) {
            fail(s,"native clear refused"); break;
        }
        (void)radar_sw3_command(c->sw3,"dma qualify");
        if(c->sw3->job==SW3_IDLE) { fail(s,"native qualification refused"); break; }
        phase(s,RADAR_SCAN_QUALIFY,"QUALIFY DMA");
        break;
    case RADAR_SCAN_QUALIFY:
        if(c->sw3->job!=SW3_IDLE) break;
        if(c->sw3->check_error || !radar_sw3_capture_ready(c->sw3)) {
            fail(s,"native qualification failed"); break;
        }
        if(s->stop_requested) { stop_between(s); break; }
        if(radar_console_free(c->console)<SCAN_LOG_ROOM) break;
        radar_sw2_command(c->sw2,"connect");
        if(c->sw2->state!=SW2_CONNECT_WAIT) { fail(s,"native connect refused"); break; }
        phase(s,RADAR_SCAN_CONNECT,"CONNECTING");
        break;
    case RADAR_SCAN_CONNECT:
        if(c->sw2->state==SW2_CONNECT_WAIT) break;
        if(c->sw2->state!=SW2_ONLINE || c->sw2->job!=SW2_JOB_NONE) {
            fail(s,"native connect failed"); break;
        }
        if(s->stop_requested) { stop_between(s); break; }
        if(radar_console_free(c->console)<SCAN_LOG_ROOM) break;
        radar_sw2_command(c->sw2,"query");
        if(c->sw2->job!=SW2_JOB_QUERY) { fail(s,"native query refused"); break; }
        phase(s,RADAR_SCAN_QUERY,"QUERYING");
        break;
    case RADAR_SCAN_QUERY:
        if(c->sw2->job!=SW2_JOB_NONE) break;
        if(c->sw2->state!=SW2_ONLINE || !c->sw2->cli_verified ||
           !c->sw2->cli.version_valid ||
           c->sw2->cli.version_epoch!=c->sw2->cli.reset_epoch ||
           !radar_capture_profile_version_matches(c->profile,c->sw2->cli.last_version)) {
            fail(s,"native query/version freshness failed"); break;
        }
        if(s->stop_requested) { stop_between(s); break; }
        if(radar_console_free(c->console)<SCAN_LOG_ROOM) break;
        start_native(s);
        break;
    default:
        fail(s,"scheduler state");
        break;
    }
}

static int readonly_command(const char *cmd)
{
    static const char *const names[]={"help","status","progress","capture status",
        "capture profile","capture preflight","capture plan","dma status",
        "dma trace","phy status","display status"};
    unsigned i;
    for(i=0U;i<sizeof(names)/sizeof(names[0]);++i)
        if(!strcmp(cmd,names[i])) return 1;
    return 0;
}

static void print_status(RadarScan *s)
{
    RadarFreshnessNow now={0};RadarFreshness freshness;
    /* No PL-relative to PS-uptime clock correspondence is established. This
     * observer can prove expiration, but must never claim CURRENT. */
    now.observed_now_us=radar_hw_now_us();now.observed_now_valid=1U;
    freshness=radar_freshness_evaluate(&s->published_freshness,&now);
    SAY(s,"[SCOPE] %s active=%u stop=%u live=%u batch=%u/%u history=%u session=%u display=%u/%u SYS_HZ=%u\r\n",
        s->status,(unsigned)s->active,(unsigned)s->stop_requested,(unsigned)s->live_mode,
        (unsigned)s->batch_done,(unsigned)s->batch_total,(unsigned)s->count,
        (unsigned)s->last_session,(unsigned)s->presented_revision,
        (unsigned)s->revision,(unsigned)s->sys_hz);
    SAY(s,"[DSP STATE] backend=%u state=%u error=%d hw_error=%08x recoverable=%u config_epoch=%u cycles=%llu vectors=%u result_flags=%08x frame_queue=%u high_water=%u display_decoupled=%u\r\n",
        (unsigned)s->backend,(unsigned)s->dsp.state,s->dsp.error,(unsigned)s->dsp.result.error,
        (unsigned)s->dsp.recoverable,(unsigned)s->dsp_config_epoch,(unsigned long long)s->dsp.result.cycles,
        (unsigned)s->dsp.result.vector_count,(unsigned)s->dsp.result.flags,(unsigned)s->frames.count,
        (unsigned)s->frames.high_water,(unsigned)s->display_decoupled);
    SAY(s,"[PIPELINE] enabled=%u producer=%u prefetch_batches=%u raw_owner=%u copy_wait_polls=%u; RX/RAW ownership is independent\r\n",
        (unsigned)s->pipeline,(unsigned)s->producer_state,(unsigned)s->prefetched_batches,
        (unsigned)s->session->raw_store_owner,(unsigned)s->session->copy_wait_polls);
    SAY(s,"[DSP PARAMETERS] %s; baseline/enhanced thresholdQ16=%u holdQ16=%u; experimental defaults, not calibrated detector performance\r\n",
        RADAR_DSP_PARAMETER_VERSION,(unsigned)RADAR_DSP_DETECTION_THRESHOLD_Q16,(unsigned)RADAR_DSP_HOLD_THRESHOLD_Q16);
    SAY(s,"[SAMPLE FRESHNESS] frame=%u backend=%u sample_valid=%u current_eligible=%u state=%s scope=SAMPLED_ONLY source=%s origin=%u flags=%08x deadline_valid=%u valid_until_us=%llu observed_age_valid=%u observed_age_lower_bound_us=%llu spatial_valid=%u detections=%u\r\n",
        s->published_freshness.frame_id,(unsigned)s->published_freshness.backend,freshness.sample_valid,
        freshness.current_eligible,radar_freshness_name(freshness.state),radar_sample_time_name(s->published_freshness.source),
        s->published_freshness.time_origin,s->published_freshness.result_flags,s->published_freshness.deadline_valid,
        (unsigned long long)s->published_freshness.valid_until_us,freshness.observed_age_valid,
        (unsigned long long)freshness.observed_age_lower_bound_us,s->published_freshness.spatial_valid,
        s->published_freshness.detection_count);
}

static int parse_count(const char *cmd,u32 *count)
{
    const char *p=cmd+11U;
    u32 n=0U;
    if(!*p) { *count=RADAR_SCAN_DEFAULT_FRAMES; return 0; }
    if(*p++!=' ' || !*p) return -1;
    while(*p) {
        if(*p<'0' || *p>'9' || n>RADAR_SCAN_MAX_FRAMES) return -1;
        n=n*10U+(u32)(*p++-'0');
    }
    if(!n || n>RADAR_SCAN_MAX_FRAMES) return -1;
    *count=n;
    return 0;
}

static void print_data(RadarScan *s,const char *arg)
{
    const RadarRangeFrame *f;
    u32 index=0U,row,bin;
    char line[192];
    int n,used;
    if(!*arg) goto usage;
    while(*arg) {
        if(*arg<'0' || *arg>'9' || index>=RADAR_SCAN_HISTORY) goto usage;
        index=index*10U+(u32)(*arg++-'0');
    }
    if(index>=s->count) goto usage;
    if(hardware_busy(s->session) || radar_console_free(s->session->console)<4096U) {
        SAY(s,"[SCOPE BLOCK] Data output needs idle runtime and 4096 free console bytes.\r\n");
        return;
    }
    f=&s->history[index];
    if(!f->valid) { SAY(s,"[SCOPE BLOCK] History frame invalid.\r\n");return; }
    /* Validate before the first output line; comparisons also reject NaN. */
    for(bin=0U;bin<RADAR_RANGE_BINS;++bin)
        if(!(f->power_dbfs[bin]>=-160.0f && f->power_dbfs[bin]<=100.0f)) {
            SAY(s,"[SCOPE BLOCK] History numeric range invalid.\r\n");return;
        }
    SAY(s,"@SCOPEBEGIN index=%u session=%u timestamp_us=%llu bins=256 scale=milli_dbfs\r\n",
        (unsigned)index,(unsigned)f->session,(unsigned long long)f->timestamp_us);
    for(row=0U;row<16U;++row) {
        used=snprintf(line,sizeof(line),"@SCOPEPOWER %u",(unsigned)row);
        for(bin=0U;bin<16U;++bin) {
            n=snprintf(line+used,sizeof(line)-(size_t)used," %d",
                (int)(f->power_dbfs[row*16U+bin]*1000.0f));
            used+=n;
        }
        SAY(s,"%s\r\n",line);
    }
    SAY(s,"@SCOPEEND session=%u\r\n",(unsigned)f->session);
    return;
usage:
    SAY(s,"[SCOPE] scope data INDEX, oldest=0, retained history=%u.\r\n",(unsigned)s->count);
}

static int scaled_value(float value,float scale)
{
    double v=(double)value*(double)scale;
    return isfinite(v) && v>-2000000000.0 && v<2000000000.0 ? (int)v : INT32_MIN;
}

/* Bounded cache export. No DSP, hardware reads, or capture-control actions. */
static void print_spatial(RadarScan *s)
{
    const RadarSpatialFrame *f=&s->spatial_frame;
    unsigned i;
    if(hardware_busy(s->session) || radar_console_free(s->session->console)<7600U ||
       !s->count || f->session!=s->history[s->count-1U].session ||
       f->count>RADAR_SPATIAL_MAX_POINTS) {
        SAY(s,"[SCOPE BLOCK] Spatial output needs matched published data, idle runtime and7600 free console bytes.\r\n");
        return;
    }
    SAY(s,"@SPATIALBEGIN session=%u timestamp_us=%llu valid=%u error=%d calibrated=%u count=%u xyz=%u candidates=%u truncated=%u\r\n",
        (unsigned)f->session,(unsigned long long)f->timestamp_us,(unsigned)f->valid,
        (int)f->error,(unsigned)f->calibrated,(unsigned)f->count,(unsigned)f->xyz_count,
        (unsigned)f->candidate_count,(unsigned)f->truncated_count);
    SAY(s,"@SPATIALFIELDS index range_bin doppler_bin range_mm velocity_mmps az_mdeg el_mdeg horizontal_mdeg x_mm y_mm z_mm snr_mdb coherence_milli power_mdbfs flags; unknown=-2147483648\r\n");
    for(i=0U;i<f->count;++i) {
        const RadarSpatialPoint *p=&f->points[i];
        SAY(s,"@SPATIALPOINT %u %u %d %d %d %d %d %d %d %d %d %d %d %d %u\r\n",
            i,(unsigned)p->range_bin,(int)p->doppler_bin,
            scaled_value(p->range_m,1000.0f),scaled_value(p->velocity_mps,1000.0f),
            scaled_value(p->azimuth_deg,1000.0f),scaled_value(p->elevation_deg,1000.0f),
            scaled_value(p->horizontal_angle_deg,1000.0f),scaled_value(p->x_m,1000.0f),
            scaled_value(p->y_m,1000.0f),scaled_value(p->z_m,1000.0f),
            scaled_value(p->snr_db,1000.0f),scaled_value(p->coherence,1000.0f),
            scaled_value(p->power_dbfs,1000.0f),(unsigned)p->flags);
    }
    SAY(s,"@SPATIALEND session=%u\r\n",(unsigned)f->session);
}

static void print_beam(RadarScan *s,const char *arg)
{
    const RadarSpatialFrame *f=&s->spatial_frame;
    unsigned row=0U,a,group;
    char line[448];
    int used;
    if(!*arg) goto usage;
    while(*arg) {
        if(*arg<'0' || *arg>'9' || row>=RADAR_SPATIAL_MAP_RANGES) goto usage;
        row=row*10U+(unsigned)(*arg++-'0');
    }
    if(row>=RADAR_SPATIAL_MAP_RANGES) goto usage;
    if(hardware_busy(s->session) || radar_console_free(s->session->console)<2048U ||
       !s->count || !f->valid || f->session!=s->history[s->count-1U].session) {
        SAY(s,"[SCOPE BLOCK] Beam row needs matched valid spatial data and idle runtime.\r\n");return;
    }
    SAY(s,"@BEAMBEGIN session=%u range_bin=%u angles=121 scale=milli_dbfs\r\n",(unsigned)f->session,row);
    for(group=0U;group<4U;++group) {
        used=snprintf(line,sizeof(line),"@BEAMPOWER %u",group);
        for(a=group*32U;a<(group+1U)*32U && a<RADAR_SPATIAL_MAP_ANGLES;++a)
            used+=snprintf(line+used,sizeof(line)-(size_t)used," %d",scaled_value(f->heatmap_dbfs[row][a],1000.0f));
        SAY(s,"%s\r\n",line);
    }
    SAY(s,"@BEAMEND session=%u\r\n",(unsigned)f->session);return;
usage:
    SAY(s,"[SCOPE] scope beam RANGE_BIN (0..204), cached integrated beam power.\r\n");
}

static int timing_command(RadarScan *s,const char *cmd)
{
    RadarTimingLog *log=&s->timing;
    const RadarTimingRecord *r;
    const char *p;
    u32 index=0U;
    if(strncmp(cmd,"scope timing",12U) || (cmd[12] && cmd[12]!=' '))return 0;
    if(!cmd[12]) {
        SAY(s,"[TIMING] records=%u capacity=%u written=%llu overwritten=%llu published=%llu failed=%llu; PS_MONOTONIC_US and TRANSPORT_TICKS_UNMAPPED\r\n",
            log->count,RADAR_TIMING_CAPACITY,(unsigned long long)log->written,
            (unsigned long long)log->overwritten,(unsigned long long)log->published,(unsigned long long)log->failed);
        SAY(s,"[TIMING ACK] events=%llu first=%llu repeat=%llu unmatched=%llu invalid=%llu nonframe=%llu; scope timing INDEX|clear requires idle\r\n",
            (unsigned long long)log->ack_events,(unsigned long long)log->first_presented,
            (unsigned long long)log->repeat_presented,(unsigned long long)log->unmatched_acks,
            (unsigned long long)log->invalid_acks,(unsigned long long)log->nonframe_acks);
        return 1;
    }
    if(s->active || hardware_busy(s->session) || radar_dsp_busy(&s->dsp)) {
        SAY(s,"[TIMING BLOCK] Stop capture/processing before timing export or clear; no performance-period dump.\r\n");return 1;
    }
    if(!strcmp(cmd+13U,"clear")) {
        radar_timing_clear(log);SAY(s,"[TIMING CLEARED] Evidence ring only; capture/display ownership unchanged.\r\n");return 1;
    }
    p=cmd+13U;if(!*p)goto usage;
    while(*p) {
        if(*p<'0' || *p>'9' || index>=RADAR_TIMING_CAPACITY)goto usage;
        index=index*10U+(u32)(*p++-'0');
    }
    r=radar_timing_get(log,index);if(!r)goto usage;
    if(radar_console_free(s->session->console)<3072U) {
        SAY(s,"[TIMING BLOCK] Console queue busy; retry when drained.\r\n");return 1;
    }
    SAY(s,"@TIMING id=%llu index=%u frame=%u batch=%u batch_index=%u backend=%u crc=%08x frame_flags=%08x stages=%08x job=%u epoch=%u revision=%u error=%d queue=%u high_water=%u\r\n",
        (unsigned long long)r->sequence,index,r->frame_id,r->acquisition_id,r->frame_index,r->backend,r->crc32,
        r->frame_flags,r->stages,r->job_id,r->config_epoch,r->published_revision,r->error,r->queue_depth,r->queue_high_water);
    SAY(s,"@TIMINGCAP begin_ps_us=%llu armed_ps_us=%llu halt_ps_us=%llu copy_begin_ps_us=%llu copy_complete_ps_us=%llu ready_ps_us=%llu observed_ps_us=%llu\r\n",
        (unsigned long long)r->batch_begin_ps_us,(unsigned long long)r->armed_ps_us,(unsigned long long)r->halt_ps_us,
        (unsigned long long)r->copy_begin_ps_us,(unsigned long long)r->copy_complete_ps_us,
        (unsigned long long)r->ready_ps_us,(unsigned long long)r->observed_ps_us);
    SAY(s,"@TIMINGWORK begin_ps_us=%llu front_begin_ps_us=%llu front_end_ps_us=%llu submit_begin_ps_us=%llu submit_end_ps_us=%llu done_ps_us=%llu post_done_ps_us=%llu published_ps_us=%llu\r\n",
        (unsigned long long)r->processing_begin_ps_us,(unsigned long long)r->front_begin_ps_us,(unsigned long long)r->front_end_ps_us,
        (unsigned long long)r->submit_begin_ps_us,(unsigned long long)r->submit_end_ps_us,(unsigned long long)r->done_ps_us,
        (unsigned long long)r->post_done_ps_us,(unsigned long long)r->published_ps_us);
    SAY(s,"@TIMINGPL source=TRANSPORT_SYS sequence=%u bytes=%u hz=%u status=%08x first_tick=%llu last_tick=%llu valid=%u dsp_cycles=%llu; origin_not_mapped_to_PS\r\n",
        r->transport_sequence,r->transport_bytes,r->transport_hz,r->transport_status,
        (unsigned long long)r->transport_first_tick,(unsigned long long)r->transport_last_tick,
        !!(r->stages&RADAR_TIMING_TRANSPORT),(unsigned long long)r->pl_cycles);
    SAY(s,"@TIMINGACK sequence=%u frame=%u revision=%u view_revision=%u page=%u coalesced=%u presentations=%u render_begin_ps_us=%llu render_end_ps_us=%llu queue_ps_us=%llu ack_ps_us=%llu\r\n",
        r->first_ack.sequence,r->first_ack.frame_id,r->first_ack.data_revision,r->first_ack.view_revision,
        r->first_ack.page,r->first_ack.coalesced,r->presentation_count,
        (unsigned long long)r->first_ack.render_begin_ps_us,(unsigned long long)r->first_ack.render_end_ps_us,
        (unsigned long long)r->first_ack.queue_ps_us,(unsigned long long)r->first_ack.ack_ps_us);
    SAY(s,"@TIMINGEND id=%llu\r\n",(unsigned long long)r->sequence);return 1;
usage:
    SAY(s,"[TIMING] scope timing [INDEX|clear]; INDEX oldest=0, count=%u, capacity=%u.\r\n",log->count,RADAR_TIMING_CAPACITY);return 1;
}

int radar_scan_command(RadarScan *s,const char *cmd)
{
    RadarSession *c=s->session;
    u32 count;
    if(timing_command(s,cmd))return 1;
    if(!strcmp(cmd,"scope status")) { print_status(s); return 1; }
    if(!strcmp(cmd,"scope stop")) {
        if(!s->active) { print_status(s); return 1; }
        if(s->live_mode && !s->stop_requested) visible(s,"LIVE STOPPING");
        s->stop_requested=1U;
        SAY(s,"[SCOPE STOP] Finish current finite capture/processing; no next RF start.\r\n");
        if(s->state!=RADAR_SCAN_CAPTURE && s->state!=RADAR_SCAN_PROCESS &&
           !(s->pipeline && s->frames.count))
            stop_between(s);
        return 1;
    }
    if(!strcmp(cmd,"safe") || !strcmp(cmd,"disable")) {
        if(s->active) {
            timing_fail(s,RADAR_DSP_E_CANCELED);
            if(radar_dsp_busy(&s->dsp)) { (void)radar_dsp_cancel(&s->dsp);s->cancel_pending=1U; }
            else if(s->frames.lease)(void)radar_frame_queue_release(&s->frames,s->frames.lease);
            radar_range_cancel(&s->processor);
            radar_spatial_cancel(&s->spatial_processor);
            s->producer_state=RADAR_SCAN_IDLE;
            finish(s,"CANCELED");
        }
        return 0; /* Existing dispatch owns immediate cancellation and IO. */
    }
    if(s->active) {
        if(readonly_command(cmd)) return 0;
        SAY(s,"[SCOPE BLOCK] Batch/processing owns the runtime. Use scope stop or safe.\r\n");
        return 1;
    }
    if(!strcmp(cmd,"scope dsp recover")) {
        int recovered=radar_dsp_recover(&s->dsp);
        if(!recovered) { s->cancel_pending=1U;SAY(s,"[DSP RECOVER] Explicit local engine recovery; original capture/metadata unchanged. Polling to idle.\r\n"); }
        else SAY(s,"[DSP RECOVER REFUSED] rc=%d; requires confirmed halted hardware lock, never busy timeout.\r\n",recovered);
        return 1;
    }
    if(radar_dsp_busy(&s->dsp)) {
        if(readonly_command(cmd))return 0;
        SAY(s,"[SCOPE BLOCK] DSP memory lease awaiting quiescence; no new capture/workspace use.\r\n");return 1;
    }
    if(!strncmp(cmd,"scope backend ",14U)) {
        RadarDspBackend selected;
        if(!strcmp(cmd+14U,"PS_REFERENCE"))selected=PS_REFERENCE;
        else if(!strcmp(cmd+14U,"PL_BASELINE"))selected=PL_BASELINE;
        else if(!strcmp(cmd+14U,"PL_ENHANCED"))selected=PL_ENHANCED;
        else { SAY(s,"[SCOPE] scope backend PS_REFERENCE|PL_BASELINE|PL_ENHANCED\r\n");return 1; }
        if(hardware_busy(c) || s->frames.count) { SAY(s,"[SCOPE BLOCK] Backend change needs idle empty frame queue.\r\n");return 1; }
        if(selected!=PS_REFERENCE) {
            int probe=radar_dsp_probe(&s->dsp);
            if(probe) { SAY(s,"[SCOPE BLOCK] PL capability/ABI unavailable rc=%d; backend unchanged.\r\n",probe);return 1; }
        }
        if(s->dsp_config_epoch==UINT32_MAX) { SAY(s,"[SCOPE BLOCK] DSP epoch exhausted.\r\n");return 1; }
        s->backend=selected;++s->dsp_config_epoch;s->previous_dsp_time=0U;s->have_previous_dsp_time=0U;
        SAY(s,"[DSP BACKEND] %s; explicit selection, no fallback, no RF command.\r\n",cmd+14U);return 1;
    }
    if(!strncmp(cmd,"scope display ",14U)) {
        if(!strcmp(cmd+14U,"decoupled"))s->display_decoupled=1U;
        else if(!strcmp(cmd+14U,"gated") && !s->pipeline)s->display_decoupled=0U;
        else { SAY(s,"[SCOPE] scope display gated|decoupled\r\n");return 1; }
        SAY(s,"[SCOPE DISPLAY MODE] %s; data publication and actual DP ACK remain distinct.\r\n",cmd+14U);return 1;
    }
    if(!strncmp(cmd,"scope pipeline ",15U)) {
        if(hardware_busy(c) || s->frames.count || c->raw_store_owner || c->result.valid) {
            SAY(s,"[SCOPE BLOCK] Pipeline change needs idle empty capture and frame queue.\r\n");return 1;
        }
        if(!strcmp(cmd+15U,"on") && s->display_decoupled &&
           (c->profile->packet_count==8U || c->profile->packet_count==16U))s->pipeline=1U;
        else if(!strcmp(cmd+15U,"off"))s->pipeline=0U;
        else { SAY(s,"[SCOPE] scope pipeline on|off; on requires batch8/16 and display decoupled.\r\n");return 1; }
        SAY(s,"[PIPELINE] enabled=%u. Finite RX prefetch; CPU RAW copy waits for consumer release. No RF started.\r\n",(unsigned)s->pipeline);return 1;
    }
    if(!strncmp(cmd,"scope data ",11U)) { print_data(s,cmd+11U);return 1; }
    if(s->frames.count && !strcmp(cmd,"capture clear")) {
        SAY(s,"[SCOPE BLOCK] Unprocessed batch frames retained. Use scope show or explicitly scope clear before capture clear.\r\n");return 1;
    }
    if(c->raw_store_owner && !strcmp(cmd,"capture once")) {
        SAY(s,"[SCOPE BLOCK] Retained RAW consumer lease needs explicit scope clear after native recovery.\r\n");return 1;
    }
    if(!strcmp(cmd,"scope spatial")) { print_spatial(s);return 1; }
    if(!strncmp(cmd,"scope beam ",11U)) { print_beam(s,cmd+11U);return 1; }
    if(!strcmp(cmd,"scope clear")) {
        if(hardware_busy(c)) { SAY(s,"[SCOPE BLOCK] Runtime busy.\r\n"); return 1; }
        if(s->dsp_config_epoch==UINT32_MAX) { SAY(s,"[SCOPE BLOCK] DSP configuration epoch exhausted.\r\n");return 1; }
        ++s->dsp_config_epoch;s->previous_dsp_time=0U;s->have_previous_dsp_time=0U;
        s->count=s->last_session=s->last_acquisition=s->sys_hz=0U;
        s->origin_observed_us=0U;
        s->transport_origin_tick=0U;s->transport_origin_us=0U;s->transport_hz=0U;
        (void)radar_frame_queue_clear(&s->frames);
        if(c->raw_store_owner)(void)radar_session_release_raw(c,c->raw_store_owner);
        memset(&s->consumer_result,0,sizeof s->consumer_result);
        s->producer_state=RADAR_SCAN_IDLE;
        s->origin_tick=s->last_tick=0U;
        s->have_origin=s->stop_requested=0U;
        s->live_mode=0U;
        s->resume_empty=0U;
        s->batch_total=s->batch_done=0U;
        s->published_session=0U;
        s->published_observed_us=0U;
        memset(&s->published_freshness,0,sizeof s->published_freshness);
        memset(&s->spatial_frame,0,sizeof(s->spatial_frame));
        radar_spatial_cancel(&s->spatial_processor);
        visible(s,"CLEARED");
        SAY(s,"[SCOPE CLEARED] Plot/history only; retained raw capture unchanged.\r\n");
        return 1;
    }
    if(!strcmp(cmd,"scope show")) {
        if(!retained_ok(c)) {
            SAY(s,"[SCOPE BLOCK] Need valid unowned finite-profile DONE result, no export, and matching confirmation.\r\n");
            return 1;
        }
        if(c->result.session<=s->last_acquisition && !s->frames.count) {
            visible(s,"RETAINED FRAME");
            SAY(s,"[SCOPE] Redisplay retained session; no duplicate appended.\r\n");
            return 1;
        }
        if(read_rate(s)) return 1;
        s->active=1U;
        s->batch_mode=s->stop_requested=s->live_mode=s->resume_empty=0U;
        s->batch_total=s->batch_done=0U;
        (void)begin_processing(s);
        return 1;
    }
    if(!strcmp(cmd,"scope live")) {
        int warm_empty=0;
        if(s->pipeline && c->profile->packet_count==1U) { SAY(s,"[SCOPE BLOCK] Pipeline requires batch8/16.\r\n");return 1; }
        if(c->result.valid) {
            /* A displayed/processed retained result may resume directly.
             * An unseen result is evidence: never silently discard it. */
            if(!retained_ok(c) || !s->count || !s->have_origin ||
               c->result.session!=s->last_acquisition ||
               s->history[s->count-1U].session!=s->last_session ||
               !s->history[s->count-1U].valid ||
               c->result.crc32!=s->source_crc ||
               c->result.snapshot.first_sys_tick!=s->last_tick) {
                SAY(s,"[SCOPE BLOCK] Retained frame has not been processed; use scope show before scope live.\r\n");
                return 1;
            }
            /* Full native preflight follows requalification/query. Check
             * persistent admission and SAFE ownership before discarding the
             * already processed raw result. No recovery or reset is hidden. */
            if(!resume_safe(s)) {
                SAY(s,"[SCOPE BLOCK] Resume needs native SAFE state and intact admission; no automatic recovery.\r\n");
                return 1;
            }
        } else if(!strcmp(s->status,"LIVE STOPPED")) {
            /* Stop may arrive after clear during QUALIFY/CONNECT/QUERY. The
             * processed history survives, while the raw result is invalid.
             * This explicit warm resume never recovers FAILED or unknown IO. */
            if(c->state!=RADAR_SESSION_IDLE || !resume_safe(s)) {
                SAY(s,"[SCOPE BLOCK] Warm resume needs known stopped history and native IDLE/SAFE admission.\r\n");
                return 1;
            }
            warm_empty=1;
        } else if(!profile_ok(c) || radar_session_preflight(c)) {
            radar_session_print_preflight(c);
            SAY(s,"[SCOPE BLOCK] First live frame needs the existing native admission gates.\r\n");
            return 1;
        }
        if(read_rate(s)) return 1;
        s->active=s->batch_mode=s->live_mode=1U;
        s->resume_empty=(u8)warm_empty;
        s->stop_requested=0U;
        s->batch_total=s->batch_done=0U;
        SAY(s,"[SCOPE LIVE] Repeated finite frames until scope stop/safe/error; preserved history=%u. No automatic retry.\r\n",
            (unsigned)s->count);
        phase(s,(c->result.valid || warm_empty) ? RADAR_SCAN_WAIT_FRAME_PRESENT :
              RADAR_SCAN_WAIT_INITIAL_PRESENT,"LIVE");
        return 1;
    }
    if(!strncmp(cmd,"scope start",11U)) {
        if(s->pipeline && c->profile->packet_count==1U) { SAY(s,"[SCOPE BLOCK] Pipeline requires batch8/16.\r\n");return 1; }
        if(parse_count(cmd,&count)) {
            SAY(s,"[SCOPE] Usage: scope start [1..32], default 16.\r\n"); return 1;
        }
        if(c->result.valid) {
            SAY(s,"[SCOPE BLOCK] Retained raw frame exists. Use scope show/export, then explicit capture clear before starting.\r\n");
            return 1;
        }
        if(!profile_ok(c) || radar_session_preflight(c)) {
            radar_session_print_preflight(c);
            SAY(s,"[SCOPE BLOCK] Fixed finite profile and existing native admission gates required.\r\n");
            return 1;
        }
        if(count%c->profile->packet_count) { SAY(s,"[SCOPE BLOCK] Requested frames must be a whole source batch.\r\n");return 1; }
        if(read_rate(s)) return 1;
        s->active=s->batch_mode=1U;
        s->stop_requested=s->live_mode=s->resume_empty=0U;
        s->batch_total=count;
        s->batch_done=0U;
        SAY(s,"[SCOPE START] %u finite frames; preserved history=%u. Measured times; no automatic retry.\r\n",
            (unsigned)count,(unsigned)s->count);
        phase(s,RADAR_SCAN_WAIT_INITIAL_PRESENT,"WAIT DISPLAY");
        return 1;
    }
    if(!strncmp(cmd,"scope",5U) && (!cmd[5] || cmd[5]==' ')) {
        SAY(s,"[SCOPE] scope live | start [1..32] | show | stop | clear | status | data INDEX | timing [INDEX/clear] | backend PS_REFERENCE/PL_BASELINE/PL_ENHANCED | display gated/decoupled | pipeline on/off | dsp recover\r\n");
        return 1;
    }
    return 0;
}
