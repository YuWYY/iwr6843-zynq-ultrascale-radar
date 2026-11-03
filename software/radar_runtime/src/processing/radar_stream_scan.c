/* SPDX-License-Identifier: MIT
 * Two immutable leased task slots, one PL engine and one cooperative PS
 * postprocessor. Capture owns RX/RAW; display owns separate snapshot slots.
 */
#include "radar_stream_scan.h"
#include "radar_crc32.h"
#include "radar_hw.h"
#include "radar_display.h"
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <stdarg.h>
enum { FREE=0,PREPARE,READY,ENGINE,POST_READY,IMPORT,SPATIAL,SCENE,RECORD };
_Static_assert(sizeof(RadarStreamRecord)==RADAR_RECORD_TIMING_BYTES,"recorded timing ABI");
#define SAY(p,...) radar_console_printf((p)->scan->session->console,__VA_ARGS__)
static void long_log(RadarStreamScan *p,const char *format,...)
{
    va_list args;int n;unsigned offset=0U;
    va_start(args,format);n=vsnprintf(p->export_line,sizeof p->export_line,format,args);va_end(args);
    if(n<0 || (size_t)n>=sizeof p->export_line || radar_console_free(p->scan->session->console)<(unsigned)n) {
        ++p->scan->session->console->dropped;return;
    }
    /* Preserve a single physical telemetry line through the console's512B
     * formatter. No send/flush wait; admitted whole line fits its TX queue. */
    while(offset<(unsigned)n) {
        unsigned count=(unsigned)n-offset;if(count>400U)count=400U;
        SAY(p,"%.*s",(int)count,p->export_line+offset);offset+=count;
    }
}
static int hsi(void *ctx,const void *raw,u32 n,u64 *id)
{ (void)ctx;return radar_range_validate_frame(raw,n,id); }
static int time_begin(void *ctx,u32 epoch)
{
    RadarStreamScan *p=ctx;int rc=radar_time_bridge_begin(&p->time,epoch);
    if(rc && p->time.io.mapped && p->time.io.read &&
       p->time.io.read(p->time.io.context,4U)==RADAR_TIME_ABI &&
       (p->time.io.read(p->time.io.context,RADAR_TIME_STATUS)&1U) &&
       p->time.io.read(p->time.io.context,RADAR_TIME_CURRENT_EPOCH)==epoch)
        p->capture.metadata_open=1U; /* Retain failed BEGIN ownership for explicit recovery. */
    return rc;
}
static int time_pop(void *ctx,RadarTransportTime *t,u64 *first,u32 *error)
{
    RadarStreamScan *p=ctx;int rc=radar_dsp_metadata_pop(&p->scan->dsp,t);
    *first=0U;*error=UINT32_MAX;
    if(rc>0 && t->valid) {
        int mapped=radar_time_bridge_map(&p->time,p->time.epoch,t->first_tick,
            radar_hw_now_us(),first,error);
        if(mapped)return mapped;
    }
    return rc;
}
static int time_end(void *ctx,u32 epoch)
{ RadarStreamScan *p=ctx;return radar_time_bridge_end(&p->time,epoch,1); }
static void visible(RadarStreamScan *p,const char *status)
{ p->scan->status=status;if(!++p->scan->revision)++p->scan->revision; }
static int tasks_busy(const RadarStreamScan *p)
{ return p->task[0].state!=FREE || p->task[1].state!=FREE; }
static uint32_t capacity(const RadarStreamScan *p)
{return p->log_capacity?p->log_capacity:RADAR_STREAM_LOG_CAPACITY;}
int radar_stream_scan_evidence_held(const RadarStreamScan *p)
{return p && radar_scene_record_holds(&p->recorder);}
static int idle(const RadarStreamScan *p)
{
    RadarSession *s=p->scan->session;
    return !p->running && !p->exporting && !p->record_exporting && !p->scan->active && !s->owns && !s->exporting &&
        !tasks_busy(p) && !radar_dsp_busy(&p->scan->dsp) && !s->control->pending &&
        s->sw3->job==SW3_IDLE && s->sw2->job==SW2_JOB_NONE &&
        s->sw2->state!=SW2_CONNECT_WAIT && !radar_cli_busy(&s->sw2->cli);
}
int radar_stream_scan_init(RadarStreamScan *p,RadarScan *scan,RadarStreamRecord *log,
                           void *scene_workspace,size_t bytes,RadarSpatialWeights *weights)
{
    RadarStreamIo io;unsigned i;
    if(!p || !scan || !log || !scene_workspace || !weights)return -1;
    memset(p,0,sizeof *p);p->scan=scan;p->log=log;p->engine_slot=p->post_slot=-1;
    p->log_capacity=RADAR_STREAM_LOG_CAPACITY;
    radar_scene_record_init(&p->recorder,log,RADAR_RECORD_REGION_BYTES);
    p->installation_epoch=1U;
    io.context=p;io.validate_frame=hsi;io.transport_begin=time_begin;
    io.transport_pop=time_pop;io.transport_end=time_end;
    radar_stream_init(&p->capture,scan->session,&io);
    radar_time_bridge_init(&p->time,&scan->dsp.io);
    for(i=0;i<2U;++i)radar_range_init(&p->task[i].range);
    if(radar_scene_init(&p->scene,scene_workspace,bytes)<0)return -2;
    return radar_spatial_cache_weights(&scan->spatial_processor,weights);
}
static void fail(RadarStreamScan *p,int error)
{
    if(!p->error) {
        p->error=error;++p->errors;
        if(error==-603)++p->crc_errors;
        if(error==RADAR_STREAM_E_HSI)++p->hsi_errors;
        if(error==RADAR_SESSION_E_DMA)++p->dma_errors;
        if(error==RADAR_STREAM_E_METADATA || (error<=-451 && error>=-458))++p->metadata_errors;
        SAY(p,"[STREAM ERROR] rc=%d acquired=%u processed=%u; controlled stop, no retry\r\n",
            error,p->capture.frames_verified,p->completed);
    }
    p->stop_requested=1U;(void)radar_stream_request_stop(&p->capture,error);
    if(radar_dsp_busy(&p->scan->dsp))(void)radar_dsp_cancel(&p->scan->dsp);
    visible(p,"STREAM ERROR");
}
static RadarStreamRecord *record(RadarStreamScan *p,const RadarStreamTask *t)
{ return &p->log[t->record]; }
static int take_frame(RadarStreamScan *p,unsigned slot)
{
    RadarStreamTask *t=&p->task[slot];RadarScan *s=p->scan;
    RadarStreamRecord *r;uint64_t mapped=0U;uint32_t uncertainty=0U;int rc;
    if(radar_scene_record_holds(&p->recorder) && p->log_count>=capacity(p))return RADAR_RECORD_E_CAPACITY;
    rc=radar_stream_acquire(&p->capture,&t->raw);if(rc>0)return 0;if(rc<0)return rc;
    if(p->next_lease==UINT32_MAX || s->dsp_job_id==UINT32_MAX) {
        (void)radar_stream_release(&p->capture,&t->raw);return -601;
    }
    t->lease=++p->next_lease;
    memset(&t->frame,0,sizeof t->frame);t->frame.raw=(const uint8_t *)t->raw.raw;
    t->frame.bytes=t->raw.bytes;t->frame.frame_id=t->raw.frame_id;
    t->frame.acquisition_id=t->raw.capture_epoch;t->frame.capture_epoch=t->raw.capture_epoch;
    t->frame.frame_index=t->raw.transport_sequence-1U;t->frame.transport_sequence=t->raw.transport_sequence;
    t->frame.raw_slot=t->raw.slot;t->frame.raw_lease=t->raw.lease;t->frame.crc32=t->raw.crc32;
    t->frame.observed_us=t->raw.observed_us;t->frame.flags=RADAR_FRAME_HSI_VALID|RADAR_FRAME_CRC_VALID;
    memcpy(t->frame.cfg_sha256,s->session->profile->cfg_sha256,65U);
    memcpy(t->frame.profile_id,s->session->profile->profile_id,17U);
    mapped=t->raw.first_accept_us;uncertainty=t->raw.time_error_us;
    if(t->raw.transport.valid && mapped && uncertainty<=RADAR_TIME_MAX_UNCERTAINTY_US) {
        t->frame.timestamp_us=mapped;t->frame.time_error_us=uncertainty;
        t->frame.flags|=RADAR_FRAME_TIME_TRUSTED|RADAR_FRAME_TIME_TRANSPORT;
    }
    t->record=p->log_count%capacity(p);
    if(p->log_count>=capacity(p))++p->log_overwritten;
    ++p->log_count;r=record(p,t);memset(r,0,sizeof *r);
    r->cpu_us=t->raw.copy_cpu_us;
    t->state=PREPARE; /* From this point every failure retains a tracked lease. */
    r->mapped_us=mapped;r->uncertainty_us=uncertainty;r->raw_slot=t->raw.slot;r->raw_lease=t->raw.lease;
    r->task_slot=slot;r->task_lease=t->lease;
    r->timing.sequence=p->log_count;r->timing.frame_id=t->frame.frame_id;
    r->timing.acquisition_id=t->frame.acquisition_id;r->timing.frame_index=t->frame.frame_index;
    r->timing.backend=PL_BASELINE;r->timing.crc32=t->frame.crc32;r->timing.frame_flags=t->frame.flags;
    r->timing.stages=RADAR_TIMING_BEGIN|(t->raw.transport.valid?RADAR_TIMING_TRANSPORT:0U);
    r->timing.observed_ps_us=t->raw.observed_us;r->timing.ready_ps_us=t->raw.verified_us;
    r->timing.transport_sequence=t->raw.transport_sequence;r->timing.transport_first_tick=t->raw.transport.first_tick;
    r->timing.transport_last_tick=t->raw.transport.last_tick;r->timing.transport_hz=t->raw.transport.hz;
    r->timing.transport_status=t->raw.transport.status;r->timing.transport_bytes=t->raw.transport.bytes;
    r->timing.processing_begin_ps_us=r->timing.front_begin_ps_us=radar_hw_now_us();
    r->timing.queue_depth=p->capture.raw.ready;r->timing.queue_high_water=p->capture.raw.high_water;
    memset(&t->job,0,sizeof t->job);t->job.frame=t->frame;t->job.backend=PL_BASELINE;
    t->job.job_id=++s->dsp_job_id;t->job.config_epoch=s->dsp_config_epoch;
    if(radar_dsp_job_layout_slot(&t->job,slot,t->lease))return -602;
    t->crc=UINT32_MAX;t->import_row=0U;t->scene_cpu_us=0U;
    rc=radar_range_begin(&t->range,t->frame.raw,t->frame.bytes,t->frame.frame_id,t->frame.timestamp_us);
    if(rc<0)return rc;
    t->range.frame.time_untrusted=!(t->frame.flags&RADAR_FRAME_TIME_TRUSTED);
    t->state=PREPARE;return 1;
}
static int prepare(RadarStreamScan *p,RadarStreamTask *t)
{
    unsigned chirp=t->range.next_chirp;int rc;
    t->crc=radar_crc32_update(t->crc,t->frame.raw+chirp*RADAR_RANGE_RECORD_BYTES,RADAR_RANGE_RECORD_BYTES);
    rc=radar_range_pack_step(&t->range,(int32_t *)(uintptr_t)t->job.input,RADAR_DSP_COMPLEX_BYTES/4U);
    if(rc<0)return rc;
    if(t->range.next_chirp==RADAR_RANGE_CHIRPS) {
        if((t->crc^UINT32_MAX)!=t->frame.crc32)return -603;
        t->frame.flags|=t->range.frame.clipped_components?RADAR_FRAME_CLIPPED:0U;
        t->job.frame.flags=t->frame.flags;record(p,t)->timing.frame_flags=t->frame.flags;
        t->state=READY;record(p,t)->timing.front_end_ps_us=radar_hw_now_us();
        record(p,t)->timing.stages|=RADAR_TIMING_FRONT_DONE;
    }
    return 0;
}
static int begin_post(RadarStreamScan *p,unsigned slot)
{
    RadarStreamTask *t=&p->task[slot];RadarSpatialProcessor *sp=&p->scan->spatial_processor;
    const RadarDspCandidateHeader *h=(const void *)(uintptr_t)t->job.candidates_out;int rc;
    rc=radar_range_import_power(&t->range,(const uint64_t *)(uintptr_t)t->job.spectrum_out);if(rc<0)return rc;
    /* Unit calibration is explicit. Measured coefficients require an exact
     * waveform/configuration and installation binding before admission. */
    rc=radar_spatial_begin_config(sp,t->frame.frame_id,t->frame.timestamp_us,
        p->have_calibration?&p->calibration:NULL,t->frame.cfg_sha256);
    if(rc<0)return rc;
    rc=radar_spatial_import_pl_begin(sp,h,(const RadarDspCandidate *)(h+1),PL_BASELINE);
    if(rc<0)return rc;
    t->state=IMPORT;p->post_slot=(int)slot;return 0;
}
static int publish(RadarStreamScan *p,RadarStreamTask *t)
{
    RadarScan *s=p->scan;RadarStreamRecord *r=record(p,t);
    const RadarSpatialFrame *sf=radar_spatial_result(&s->spatial_processor);
    const RadarSceneFrame *scene=&t->scene_result;
    const RadarRangeFrame *range=radar_range_result(&t->range);
    if(!range || !sf || !scene || sf->session!=range->session)return -604;
    if(s->count==RADAR_SCAN_HISTORY) {
        memmove(s->history,s->history+1U,(RADAR_SCAN_HISTORY-1U)*sizeof s->history[0]);--s->count;
    }
    s->history[s->count++]=*range;s->spatial_frame=*sf;p->scene_frame=*scene;
    s->last_session=s->published_session=range->session;s->last_acquisition=t->frame.acquisition_id;
    s->next_frame_id=range->session+1U;
    s->expected_crc=t->frame.crc32;s->published_observed_us=t->frame.timestamp_us?t->frame.timestamp_us:t->frame.observed_us;
    radar_freshness_capture(&s->published_freshness,&t->frame,PL_BASELINE,t->result.flags,t->frame.capture_epoch,1);
    s->published_freshness.spatial_valid=1U;s->published_freshness.detection_count=sf->xyz_count;
    ++p->completed;s->batch_done=p->completed;
    visible(p,p->stop_requested?"LIVE STOPPING":"LIVE");
    r->timing.stages|=RADAR_TIMING_POST_DONE|RADAR_TIMING_PUBLISHED;
    r->timing.post_done_ps_us=r->timing.published_ps_us=radar_hw_now_us();
    r->timing.published_revision=s->revision;
    if(radar_stream_release(&p->capture,&t->raw))return -605;
    t->state=FREE;p->post_slot=-1;return 0;
}
static int post_step(RadarStreamScan *p,RadarStreamTask *t)
{
    RadarSpatialProcessor *sp=&p->scan->spatial_processor;int rc;uint64_t scene_begin;
    if(t->state==IMPORT) {
        unsigned row=t->import_row;
        rc=radar_spatial_import_pl_row(sp,row,(const int32_t *)(uintptr_t)t->job.cube_out+row*12U*32U*2U,
            (const uint64_t *)(uintptr_t)t->job.rd_out+row*32U);
        if(rc<0)return rc;
        if(++t->import_row==256U)t->state=SPATIAL;
        return 0;
    }
    if(t->state==SPATIAL) {
        rc=radar_spatial_step(sp);if(rc<0)return rc;
        if(rc==RADAR_SPATIAL_DONE) {
            RadarSceneContext context;memset(&context,0,sizeof context);
            context.capture_epoch=t->frame.capture_epoch;context.frame_id=t->frame.frame_id;
            context.installation_epoch=p->installation_epoch;context.timestamp_us=t->frame.timestamp_us;
            context.time_valid=!!(t->frame.flags&RADAR_FRAME_TIME_TRUSTED);
            context.frame_flags=t->job.frame.flags;
            memcpy(context.cfg_sha256,t->frame.cfg_sha256,65U);
            memcpy(context.calibration_id,sp->frame.calibration_id,sizeof context.calibration_id);
            scene_begin=radar_hw_now_us();rc=radar_scene_begin(&p->scene,&sp->frame,&context);
            t->scene_cpu_us+=radar_hw_now_us()-scene_begin;if(rc<0)return rc;
            t->state=SCENE;
        }
        return 0;
    }
    if(t->state==RECORD) {
        rc=radar_scene_record_step(&p->recorder);if(rc<0)return rc;
        return rc==RADAR_RECORD_DONE?publish(p,t):0;
    }
    scene_begin=radar_hw_now_us();rc=radar_scene_step(&p->scene);
    t->scene_cpu_us+=radar_hw_now_us()-scene_begin;if(rc<0)return rc;
    if(rc!=RADAR_SCENE_DONE)return 0;
    t->scene_result=*radar_scene_result(&p->scene);
    t->scene_result.diagnostics.processing_us=t->scene_cpu_us>UINT32_MAX?UINT32_MAX:(uint32_t)t->scene_cpu_us;
    if(t->scene_cpu_us>5000U)++p->scene_budget_overruns;
    t->scene_result.diagnostics.budget_overruns=p->scene_budget_overruns;
    if(p->recorder.state==RADAR_RECORD_ACTIVE || p->recorder.state==RADAR_RECORD_DRAINING) {
        rc=radar_scene_record_begin(&p->recorder,&t->frame,radar_spatial_result(sp),
            &t->scene_result,radar_range_result(&t->range),t->result.flags);
        if(rc<0)return rc;
        if(rc==RADAR_RECORD_BUSY){t->state=RECORD;return 0;}
    }
    return publish(p,t);
}
static void export_poll(RadarStreamScan *p)
{
    RadarStreamRecord *r;RadarTimingRecord *t;
    if(!p->exporting || radar_console_free(p->scan->session->console)<1700U)return;
    if(!p->export_remaining) { p->exporting=0U;SAY(p,"@STREAMEND next=%u records=%u epoch=%u\r\n",p->export_index,p->log_count,p->time.epoch);return; }
    r=&p->log[p->export_index%capacity(p)];t=&r->timing;
    long_log(p,"@STREAM record=%llu frame=%u epoch=%u seq=%u crc=%08x flags=%x stages=%x raw=%u lease=%u task=%u tasklease=%u first=%llu last=%llu mapped=%llu uncertainty=%u observed=%llu ready=%llu front=%llu submitted=%llu done=%llu published=%llu ack=%llu cpu=%llu render=%llu queue=%u high=%u revision=%u page=%u error=%d bytes=%u hz=%u tstatus=%x ackseq=%u ackrevision=%u ackframe=%u renderbegin=%llu renderend=%llu queued=%llu coalesced=%u\r\n",
        (unsigned long long)t->sequence,t->frame_id,t->acquisition_id,t->transport_sequence,t->crc32,t->frame_flags,t->stages,
        r->raw_slot,r->raw_lease,r->task_slot,r->task_lease,
        (unsigned long long)t->transport_first_tick,(unsigned long long)t->transport_last_tick,
        (unsigned long long)r->mapped_us,r->uncertainty_us,(unsigned long long)t->observed_ps_us,
        (unsigned long long)t->ready_ps_us,(unsigned long long)t->front_end_ps_us,
        (unsigned long long)t->submit_end_ps_us,(unsigned long long)t->done_ps_us,
        (unsigned long long)t->published_ps_us,(unsigned long long)t->first_ack.ack_ps_us,
        (unsigned long long)r->cpu_us,
        (unsigned long long)(t->first_ack.render_end_ps_us-t->first_ack.render_begin_ps_us),
        t->queue_depth,t->queue_high_water,t->published_revision,t->first_ack.page,t->error,
        t->transport_bytes,t->transport_hz,t->transport_status,t->first_ack.sequence,t->first_ack.data_revision,t->first_ack.frame_id,
        (unsigned long long)t->first_ack.render_begin_ps_us,(unsigned long long)t->first_ack.render_end_ps_us,
        (unsigned long long)t->first_ack.queue_ps_us,t->first_ack.coalesced);
    ++p->export_index;--p->export_remaining;
}
static void record_export_poll(RadarStreamScan *p)
{
    uint8_t data[128];char hex[257];unsigned n,i;uint32_t crc;int rc;
    static const char digits[]="0123456789abcdef";
    if(!p->record_exporting || radar_console_free(p->scan->session->console)<1024U)return;
    if(p->record_export_offset==p->record_export_limit) {
        rc=SAY(p,"@SCENEEND epoch=%u next=%u bytes=%u crc=%08x\r\n",p->recorder.capture_epoch,p->record_export_limit,p->recorder.image_bytes,p->recorder.image_crc);
        if(!rc)p->record_exporting=0U;
        return;
    }
    n=p->record_export_limit-p->record_export_offset;if(n>sizeof data)n=sizeof data;
    rc=radar_scene_record_read(&p->recorder,p->record_export_offset,data,n);
    if(rc){p->record_exporting=0U;SAY(p,"[RECORD EXPORT FAIL] rc=%d\r\n",rc);return;}
    for(i=0;i<n;++i){hex[2U*i]=digits[data[i]>>4U];hex[2U*i+1U]=digits[data[i]&15U];}hex[2U*n]=0;
    crc=radar_crc32_update(UINT32_MAX,data,n)^UINT32_MAX;
    rc=SAY(p,"@SCENEDATA epoch=%u offset=%u bytes=%u crc=%08x %s\r\n",
        p->recorder.capture_epoch,p->record_export_offset,n,crc,hex);
    if(!rc)p->record_export_offset+=n;
}
static int record_quiet(const RadarStreamScan *p)
{
    const RadarSession *s=p->scan->session;RadarDma *d=&s->sw3->dma;
    return !p->running && !p->scan->active && !radar_stream_active(&p->capture) && !tasks_busy(p) &&
        !radar_dsp_busy(&p->scan->dsp) && !s->owns && s->source==RADAR_SOURCE_STOPPED &&
        s->state==RADAR_SESSION_IDLE && !p->capture.metadata_open && !p->capture.raw.used &&
        !d->lease_active && !d->packet_queued && d->state==RADAR_DMA_READY && d->reset_confirmed &&
        radar_dma_is_quiescent(d);
}
static void record_idle_poll(RadarStreamScan *p)
{
    RadarSceneRecorder *r=&p->recorder;uint32_t skipped;int tail,rc;
    if((r->state==RADAR_RECORD_ACTIVE || r->state==RADAR_RECORD_DRAINING) && record_quiet(p)) {
        tail=!p->completed || (p->latest_ack_frame==p->scan->published_session && !p->stop_view_pending);
        skipped=p->completed>p->presented?p->completed-p->presented:0U;
        rc=radar_scene_record_freeze(r,p->log_count,p->presented,skipped,p->dp_fault_bits,
            p->scan->session->source,1,tail,p->error);
        if(!rc)SAY(p,"[RECORD] HASHING frames=%u timing=%u heatmaps=%u; source/DMA/RAW/tasks quiet, final actual ACK\r\n",r->count,r->timing_count,r->heat_count);
    }
    if(r->state==RADAR_RECORD_HASHING) {
        rc=radar_scene_record_hash_step(r);
        if(rc==RADAR_RECORD_DONE)SAY(p,"[RECORD FROZEN] frames=%u bytes=%u crc=%08x incomplete=%u error=%d; export retained immutable evidence\r\n",
            r->count,r->image_bytes,r->image_crc,r->incomplete,r->error);
    }
    record_export_poll(p);
}
void radar_stream_scan_poll(RadarStreamScan *p)
{
    RadarScan *s=p->scan;unsigned i,n;uint64_t now=radar_hw_now_us(),begin;int rc;
    if(!p->running) {
        if(p->stop_view_pending && p->log_count) {
            const RadarTimingRecord *r=&p->log[(p->log_count-1U)%capacity(p)].timing;
            if(r->first_ack.sequence) {
                p->stop_view_pending=0U;visible(p,p->error?"STREAM ERROR":"LIVE STOPPED");
            }
        }
        record_idle_poll(p);export_poll(p);return;
    }
    if(p->capture.error && !p->error)fail(p,p->capture.error);
    if(p->capture.metadata_open && p->time.valid && now-p->time.anchor_us>=RADAR_TIME_REFRESH_US) {
        rc=radar_time_bridge_refresh(&p->time);if(rc)fail(p,rc);
    }
    if(p->engine_slot>=0) {
        RadarStreamTask *t=&p->task[p->engine_slot];begin=radar_hw_now_us();
        rc=radar_dsp_poll(&s->dsp);record(p,t)->cpu_us+=radar_hw_now_us()-begin;
        if(rc<0 && !p->error)fail(p,rc);
        if(rc==RADAR_DSP_DONE) {
            const RadarDspResult *result=radar_dsp_result(&s->dsp);
            if(!result)fail(p,-606);
            else {
                t->result=*result;t->state=POST_READY;record(p,t)->timing.done_ps_us=radar_hw_now_us();
                record(p,t)->timing.pl_cycles=result->cycles;record(p,t)->timing.stages|=RADAR_TIMING_DONE;
            }
            p->engine_slot=-1;
        } else if(rc<0 && !radar_dsp_busy(&s->dsp))p->engine_slot=-1;
    }
    if(p->error) {
        /* No output is published after failure. Wait for engine quiescence
         * before returning RAW/task leases; a locked engine retains owners. */
        if(!radar_dsp_busy(&s->dsp)) {
            radar_spatial_cancel(&s->spatial_processor);radar_scene_cancel(&p->scene);
            radar_scene_record_cancel_pending(&p->recorder,p->error);
            for(i=0;i<2U;++i)if(p->task[i].state!=FREE) {
                record(p,&p->task[i])->timing.error=p->error;
                record(p,&p->task[i])->timing.stages|=RADAR_TIMING_FAILED;
                radar_range_cancel(&p->task[i].range);
                (void)radar_stream_release(&p->capture,&p->task[i].raw);p->task[i].state=FREE;
            }
            p->post_slot=-1;
        }
    } else {
        for(i=0;i<2U;++i)if(p->task[i].state==FREE) {
            rc=take_frame(p,i);if(rc<0) { fail(p,rc);break; }
        }
        /* Preparation and postprocessing each have a bounded CPU slice. The
         * main loop services RX/UART/DP between slices, never waits for PL. */
        for(i=0;i<2U && !p->error;++i)if(p->task[i].state==PREPARE) {
            RadarStreamTask *t=&p->task[i];begin=radar_hw_now_us();
            for(n=0;n<8U && t->state==PREPARE;++n) {
                rc=prepare(p,t);if(rc<0) { fail(p,rc);break; }
                if(radar_hw_now_us()-begin>=500U)break;
            }
            record(p,t)->cpu_us+=radar_hw_now_us()-begin;
        }
        if(p->engine_slot<0 && !p->error) {
            int next=-1;
            for(i=0;i<2U;++i)if(p->task[i].state==READY &&
                (next<0 || p->task[i].frame.frame_id<p->task[next].frame.frame_id))next=(int)i;
            if(next>=0) {
                RadarStreamTask *t=&p->task[next];RadarTimingRecord *r=&record(p,t)->timing;
                begin=radar_hw_now_us();r->submit_begin_ps_us=begin;
                rc=radar_dsp_submit(&s->dsp,&t->job);record(p,t)->cpu_us+=radar_hw_now_us()-begin;
                if(rc)fail(p,rc);
                else { r->submit_end_ps_us=radar_hw_now_us();r->stages|=RADAR_TIMING_SUBMITTED;
                    r->job_id=t->job.job_id;r->config_epoch=t->job.config_epoch;t->state=ENGINE;p->engine_slot=next; }
            }
        }
        if(p->post_slot<0 && !p->error)for(i=0;i<2U;++i)if(p->task[i].state==POST_READY) {
            begin=radar_hw_now_us();rc=begin_post(p,i);record(p,&p->task[i])->cpu_us+=radar_hw_now_us()-begin;
            if(rc)fail(p,rc);
            break;
        }
        if(p->post_slot>=0 && !p->error) {
            RadarStreamTask *t=&p->task[p->post_slot];begin=radar_hw_now_us();
            for(n=0;n<256U;++n) {
                rc=post_step(p,t);if(rc<0) { fail(p,rc);break; }
                if(t->state==FREE || radar_hw_now_us()-begin>=500U)break;
            }
            record(p,t)->cpu_us+=radar_hw_now_us()-begin;
        }
    }
    if(radar_scene_record_holds(&p->recorder) && !p->stop_requested &&
       (p->recorder.stop_due || p->log_count>=RADAR_RECORD_CAPACITY-64U)) {
        int reason=p->recorder.error;
        if(!reason && p->log_count>=RADAR_RECORD_CAPACITY-64U)reason=RADAR_RECORD_E_CAPACITY;
        p->stop_requested=1U;radar_scene_record_stop_requested(&p->recorder,reason);
        rc=radar_stream_request_stop(&p->capture,reason);if(rc && radar_stream_active(&p->capture))fail(p,rc);
        SAY(p,"[RECORD STOP REQUEST] frames=%u duration_s=%u reason=%d; existing packet-boundary stop\r\n",p->recorder.count,p->recorder.duration_s,reason);
    }
    now=radar_hw_now_us();
    if(now-p->last_summary_us>=1000000U && radar_console_free(s->session->console)>=1024U) {
        SAY(p,"[STREAM LIVE] acquired=%u processed=%u presented=%u pending=%u high=%u errors=%u mapped=%d elapsed_ms=%llu\r\n",
            p->capture.frames_verified,p->completed,p->presented,p->capture.raw.used,p->capture.raw.high_water,p->errors,p->time.valid,
            (unsigned long long)((now-p->begin_us)/1000U));p->last_summary_us=now;
    }
    if(!radar_stream_active(&p->capture) && !tasks_busy(p) &&
       (!p->capture.raw.ready || p->error) && !radar_dsp_busy(&s->dsp)) {
        p->running=0U;s->active=0U;s->state=RADAR_SCAN_IDLE;
        /* A status-only revision must not supersede the final data revision
         * before its first real ACK. Otherwise a complete final frame would
         * disappear from the timing evidence despite being rendered. */
        p->stop_view_pending=0U;
        if(!p->error && p->log_count) {
            const RadarTimingRecord *r=&p->log[(p->log_count-1U)%capacity(p)].timing;
            if((r->stages&RADAR_TIMING_PUBLISHED) && !r->first_ack.sequence)p->stop_view_pending=1U;
        }
        if(p->stop_view_pending)s->status="LIVE STOPPED";
        else visible(p,p->error?"STREAM ERROR":"LIVE STOPPED");
        SAY(p,"[STREAM STOPPED] verified=%u processed=%u presented=%u remaining=%u residual=%u error=%d records=%u; evidence retained\r\n",
            p->capture.frames_verified,p->completed,p->presented,p->capture.raw.ready,p->capture.frames_residual,p->error,p->log_count);
    }
}
void radar_stream_scan_ack(RadarStreamScan *p,const RadarTimingAck *a)
{
    uint32_t i,n;
    if(!a || !a->sequence || a->sequence==p->last_ack || p->recorder.state>=RADAR_RECORD_HASHING)return;
    p->last_ack=a->sequence;p->coalesced=a->coalesced;
    n=p->log_count<capacity(p)?p->log_count:capacity(p);
    for(i=0;i<n;++i) {
        RadarStreamRecord *r=&p->log[(p->log_count-1U-i)%capacity(p)];
        if(r->timing.frame_id==a->frame_id && r->timing.published_revision==a->data_revision &&
           (r->timing.stages&RADAR_TIMING_PUBLISHED)) {
            if(!r->timing.first_ack.sequence) {
                r->timing.first_ack=*a;r->timing.stages|=RADAR_TIMING_ACKED;++p->presented;
                p->latest_ack_frame=a->frame_id;
                if(a->queue_ps_us>=a->render_begin_ps_us)
                    r->cpu_us+=a->queue_ps_us-a->render_begin_ps_us;
                if(p->last_ack_us && a->ack_ps_us>=p->last_ack_us && a->ack_ps_us-p->last_ack_us>p->max_gap_us)
                    p->max_gap_us=a->ack_ps_us-p->last_ack_us;
                p->last_ack_us=a->ack_ps_us;
            }
            return;
        }
    }
}
static int select_profile(RadarStreamScan *p,uint32_t frames)
{
    RadarSession *s=p->scan->session;const RadarCaptureProfile *profile;
    if(!idle(p) || radar_scene_record_holds(&p->recorder) || s->result.valid || s->raw_store_owner || p->capture.raw.used || p->capture.metadata_open)return -1;
    profile=radar_capture_profile_stream(frames);if(!profile)return -1;
    s->profile=profile;s->confirmed=0U;p->frame_limit=frames;
    /* Changing acquisition identity invalidates cfg reuse and all admission. */
    radar_session_invalidate_config(s);
    SAY(p,"[CAPTURE MODE] STREAM frames=%u profile=%s; capture confirm ID required, no RF started\r\n",frames,profile->profile_id);
    return 0;
}
static int start(RadarStreamScan *p)
{
    RadarScan *s=p->scan;int rc;
    if(!idle(p) || s->backend!=PL_BASELINE || p->capture.raw.used ||
       (radar_scene_record_holds(&p->recorder) && p->recorder.state!=RADAR_RECORD_ARMED) ||
       (p->recorder.state==RADAR_RECORD_ARMED && (p->frame_limit || !p->dp_active)))return -1;
    rc=radar_dsp_probe(&s->dsp);if(rc)return rc;
    if(s->dsp_config_epoch==UINT32_MAX)return -1;
    if(s->last_session==UINT32_MAX || !p->capture.raw.next_frame_id)return -1;
    if(p->capture.raw.next_frame_id<=s->last_session)p->capture.raw.next_frame_id=s->last_session+1U;
    if(p->capture.raw.next_frame_id<s->next_frame_id)p->capture.raw.next_frame_id=s->next_frame_id;
    rc=radar_stream_start(&p->capture,p->frame_limit);if(rc)return rc;
    if(p->recorder.state==RADAR_RECORD_ARMED) {
        rc=radar_scene_record_start(&p->recorder,s->session->active_session);
        if(rc){radar_stream_cancel(&p->capture);return rc;}
    }
    ++s->dsp_config_epoch;p->running=1U;p->stop_requested=0U;p->error=0;
    p->completed=p->presented=p->errors=p->last_ack=p->log_count=p->log_overwritten=0U;
    p->stop_view_pending=0U;
    p->scene_budget_overruns=0U;
    p->crc_errors=p->hsi_errors=p->dma_errors=p->metadata_errors=p->latest_ack_frame=0U;
    p->last_ack_us=p->max_gap_us=0U;p->begin_us=p->last_summary_us=radar_hw_now_us();
    s->active=s->live_mode=s->display_decoupled=1U;s->pipeline=0U;s->batch_done=0U;
    s->batch_total=p->frame_limit;visible(p,"LIVE STARTING");
    SAY(p,"[STREAM START] frames=%u backend=PL_BASELINE; packet-first pipeline, detailed RAM log, no retry\r\n",p->frame_limit);
    return 0;
}
static int record_command(RadarStreamScan *p,const char *cmd)
{
    unsigned seconds,offset,count;char extra;int rc;RadarSceneRecorder *r=&p->recorder;
    RadarSession *s=p->scan->session;
    if(strncmp(cmd,"scope record",12U))return 0;
    if(!strcmp(cmd,"scope record status")) {
        SAY(p,"[RECORD STATUS] state=%s duration_s=%u frames=%u timing=%u heatmaps=%u missed=%u epoch=%u bytes=%u crc=%08x error=%d incomplete=%u exporting=%u next=%u\r\n",
            radar_scene_record_state_name(r->state),r->duration_s,r->count,r->timing_count,r->heat_count,r->heat_missed,r->capture_epoch,
            r->image_bytes,r->image_crc,r->error,r->incomplete,p->record_exporting,p->record_export_offset);return 1;
    }
    if(sscanf(cmd,"scope record arm %u %c",&seconds,&extra)==1) {
        RadarRecordIdentity id;
        if(!idle(p) || !p->mode || p->frame_limit || p->capture.raw.used || p->capture.metadata_open ||
           p->stop_view_pending || r->state!=RADAR_RECORD_EMPTY || !s->confirmed ||
           s->profile!=radar_capture_profile_stream(0U)) {
            SAY(p,"[RECORD BLOCK] Idle confirmed continuous STREAM, no owners/pending ACK and EMPTY recorder required\r\n");return 1;
        }
        memset(&id,0,sizeof id);id.software_version=RADAR_SW_VERSION;id.build_id=RADAR_SW_BUILD_ID;
        id.profile_id=s->profile->profile_id;id.cfg_sha256=s->profile->cfg_sha256;
        id.array_id=RADAR_SPATIAL_ARRAY_ID;id.calibration_id=p->have_calibration?p->calibration.calibration_id:"UNCALIBRATED";
        id.installation_epoch=p->installation_epoch;id.bit_sha256=RADAR_EXPECTED_BIT_SHA256;id.xsa_sha256=RADAR_EXPECTED_XSA_SHA256;
        id.reflection_id_seed=radar_scene_reflection_next_id(&p->scene);
        id.firmware_expected_sha256=s->profile->firmware_sha256;id.firmware_version_observed=s->sw2->cli.last_version;
        id.scene_parameters=radar_scene_parameter_identity();
        rc=radar_scene_record_arm(r,seconds,&id);
        if(!rc){p->log_capacity=RADAR_RECORD_CAPACITY;p->log_count=p->log_overwritten=0U;
            SAY(p,"[RECORD ARMED] seconds=%u capacity=%u; scope live starts real capture, timed normal stop, then final ACK and freeze\r\n",seconds,capacity(p));}
        else SAY(p,"[RECORD BLOCK] arm rc=%d; only60/180 seconds, frozen evidence requires explicit clear\r\n",rc);
        return 1;
    }
    if(!strncmp(cmd,"scope record export",19U)) {
        offset=0U;count=65536U;
        if(strcmp(cmd,"scope record export") && sscanf(cmd,"scope record export %u %u %c",&offset,&count,&extra)!=2) {
            SAY(p,"[RECORD] scope record export OFFSET BYTES (1..65536)\r\n");return 1;
        }
        if(!idle(p) || !record_quiet(p) || r->state!=RADAR_RECORD_FROZEN || !count || count>65536U || offset>=r->image_bytes) {
            SAY(p,"[RECORD BLOCK] FROZEN, idle unowned source/DMA and valid export page required\r\n");return 1;
        }
        if(count>r->image_bytes-offset)count=r->image_bytes-offset;
        rc=SAY(p,"@SCENEBEGIN schema=1 epoch=%u bytes=%u crc=%08x start=%u count=%u frames=%u timing=%u heatmaps=%u\r\n",
            r->capture_epoch,r->image_bytes,r->image_crc,offset,count,r->count,r->timing_count,r->heat_count);
        if(!rc){p->record_export_offset=offset;p->record_export_limit=offset+count;p->record_exporting=1U;}
        return 1;
    }
    if(!strcmp(cmd,"scope record clear")) {
        if(!idle(p) || p->capture.raw.used || p->capture.metadata_open || p->stop_view_pending ||
           (r->state==RADAR_RECORD_FROZEN && !record_quiet(p))) {
            SAY(p,"[RECORD BLOCK] Stop, finish final ACK/recovery and export before clearing retained evidence\r\n");return 1;
        }
        rc=radar_scene_record_clear(r);
        if(!rc){p->log_capacity=RADAR_STREAM_LOG_CAPACITY;p->log_count=p->log_overwritten=0U;
            SAY(p,"[RECORD CLEARED] Explicit discard; normal STREAM timing capacity8192 restored\r\n");}
        else SAY(p,"[RECORD BLOCK] clear rc=%d; ARMED or FROZEN only\r\n",rc);
        return 1;
    }
    if(!strcmp(cmd,"scope record discard")) {
        unsigned saved=r->count;int reason=r->error;
        if(!idle(p) || !record_quiet(p)) {
            SAY(p,"[RECORD BLOCK] Explicit source/DMA recovery and all owners released required before abandoning unfrozen evidence\r\n");return 1;
        }
        rc=radar_scene_record_discard(r,1);
        if(!rc){p->log_capacity=RADAR_STREAM_LOG_CAPACITY;p->log_count=p->log_overwritten=0U;p->stop_view_pending=0U;
            SAY(p,"[RECORD DISCARDED] failed_frames=%u error=%d; explicit evidence loss, no tail ACK invented, no freeze/PASS\r\n",saved,reason);}
        else SAY(p,"[RECORD BLOCK] discard rc=%d\r\n",rc);
        return 1;
    }
    SAY(p,"[RECORD] scope record arm 60|180; status; export OFFSET BYTES; clear\r\n");return 1;
}
int radar_stream_scan_command(RadarStreamScan *p,const char *cmd)
{
    RadarScan *s=p->scan;unsigned first,count;char extra;int rc;
    if(p->record_exporting) {
        SAY(p,"[RECORD BLOCK] One export page owns console output; wait for SCENEEND\r\n");return 1;
    }
    if(record_command(p,cmd))return 1;
    if(!strncmp(cmd,"capture mode ",13U)) {
        if(!idle(p) || radar_scene_record_holds(&p->recorder)) { SAY(p,"[STREAM BLOCK] Mode change requires idle unowned buffers and cleared record evidence\r\n");return 1; }
        if(!strcmp(cmd+13U,"stream")) {
            rc=select_profile(p,0U);if(!rc)p->mode=1U;else SAY(p,"[STREAM BLOCK] Retained evidence must be explicitly cleared first\r\n");
        } else if(!strcmp(cmd+13U,"finite") && !p->capture.raw.used && !p->capture.metadata_open) {
            p->mode=0U;s->session->profile=radar_capture_profile_batch(8U);s->session->confirmed=0U;
            radar_session_invalidate_config(s->session);SAY(p,"[CAPTURE MODE] FINITE; confirm selected profile\r\n");
        } else SAY(p,"[CAPTURE MODE] capture mode finite|stream; retained slots block change\r\n");
        return 1;
    }
    if(!p->mode)return 0;
    if(!strcmp(cmd,"scope status")) {
        uint32_t i,skipped=0U,pending=0U,total=p->log_count<capacity(p)?p->log_count:capacity(p);
        for(i=0;i<total;++i) {
            const RadarTimingRecord *r=&p->log[i].timing;
            if((r->stages&RADAR_TIMING_PUBLISHED) && !r->first_ack.sequence) {
                if(r->frame_id<p->latest_ack_frame)++skipped;else ++pending;
            }
        }
        long_log(p,"[STREAM STATUS] running=%u limit=%u epoch=%u verified=%u processed=%u presented=%u ready=%u owners=%u high=%u error=%d records=%u overwritten=%u time_valid=%d uncertainty_us=%u skipped_display=%u pending_display=%u crc_errors=%u hsi_errors=%u dma_errors=%u metadata_errors=%u dp_errors=%x dp_active=%u health_cpu_us=%llu health_max_us=%u\r\n",
            p->running,p->frame_limit,p->time.epoch,p->capture.frames_verified,p->completed,p->presented,p->capture.raw.ready,
            p->capture.raw.processing,p->capture.raw.high_water,p->error,p->log_count,p->log_overwritten,p->time.valid,p->time.anchor_uncertainty_us,
            skipped,pending,p->crc_errors,p->hsi_errors,p->dma_errors,p->metadata_errors,p->dp_fault_bits,p->dp_active,
            (unsigned long long)p->capture.health_cpu_us,p->capture.health_max_us);return 1;
    }
    if(!strcmp(cmd,"scope stop")) {
        radar_scene_record_stop_requested(&p->recorder,0);
        p->stop_requested=1U;rc=radar_stream_request_stop(&p->capture,0);
        SAY(p,"[STREAM STOP REQUEST] rc=%d; boundary then sensorStop, verified drain and DMA reset\r\n",rc);return 1;
    }
    if(p->error && !strcmp(cmd,"scope dsp recover")) {
        rc=radar_dsp_recover(&s->dsp);
        SAY(p,"[STREAM DSP RECOVER] rc=%d; retained leases released only after native quiescence\r\n",rc);return 1;
    }
    if(!strcmp(cmd,"scope scene") || !strcmp(cmd,"scope scene tracks")) {
        const RadarSceneFrame *f=&p->scene_frame;unsigned i;
        int tracks=!strcmp(cmd,"scope scene tracks");
        const RadarSceneDiagnostics *d=&f->diagnostics;
        if(!idle(p) || radar_console_free(s->session->console)<3800U) {
            SAY(p,"[SCENE BLOCK] Stop processing before cached rule export\r\n");return 1;
        }
        SAY(p,"@SCENE frame=%u epoch=%u time_us=%llu tracks=%u reflections=%u walls=%u bearings=%u flags=%x rules=RULES_V1 calibration=%s DIAGNOSTIC_NOT_ACCURACY\r\n",
            f->frame_id,f->capture_epoch,(unsigned long long)f->timestamp_us,f->track_count,f->reflection_count,f->wall_count,f->bearing_count,f->flags,f->calibration_id);
        if(!tracks) {
            SAY(p,"@SCENEDIAG reject=%x buckets=%u/%u elapsed_ms=%u radial=%u cells=%u independent=%u pointlike=%u overflow=%u stable=%u tentative=%u stale=%u ambiguous=%u rangeonly=%u rejected=%u cpu_us=%u overruns=%u\r\n",
                d->reject_flags,d->valid_buckets,d->required_buckets,d->map_elapsed_ms,d->radial_candidates,d->persistent_cells,
                d->independent_groups,d->point_like_groups,d->candidate_overflow,d->stable_count,d->tentative_count,d->stale_count,
                d->ambiguous_count,d->range_only_count,d->rejected_points,d->processing_us,d->budget_overruns);
            for(i=0;i<f->reflection_count && i<RADAR_SCENE_MAX_REFLECTIONS;++i) {
                const RadarStableReflection *v=&f->reflections[i];char direction[20];
                if((v->flags&RADAR_REFLECTION_U_VALID) && v->u>=-1.0f && v->u<=1.0f)
                    (void)snprintf(direction,sizeof direction,"%d",(int)(v->u*1000.0f));
                else strcpy(direction,"UNAVAILABLE");
                SAY(p,"@REFLECTION id=%u state=%u flags=%x support=%u last_us=%llu range_mm=%d median_mm=%d mad_mm=%d u_milli=%s power_mdbfs=%d snr_mdb=%d samples=%u reject=%x\r\n",
                    v->id,v->state,v->flags,v->support_buckets,(unsigned long long)v->last_observed_us,
                    (int)(v->range_m*1000.0f),(int)(v->range_median_m*1000.0f),(int)(v->range_mad_m*1000.0f),
                    direction,(int)(v->power_dbfs*1000.0f),(int)(v->snr_db*1000.0f),v->sample_count,v->reject_flags);
            }
        }
        for(i=0;tracks && i<f->track_count && i<RADAR_SCENE_MAX_TRACKS;++i) {
            const RadarSceneTrack *t=&f->tracks[i];
            SAY(p,"@TRACK id=%u measured=%u predicted=%u label=%s rule_bits=%x score=%u last_us=%llu x_mm=%d y_mm=%d speed_mmps=%d extent_mm=%d shape_mm=%d radial_spread_mmps=%d\r\n",
                t->id,t->measured,t->predicted,radar_scene_label_name(t->label),t->rule_bits,t->rule_score,
                (unsigned long long)t->last_measured_us,(int)(t->x_m*1000.0f),(int)(t->y_m*1000.0f),
                (int)(t->motion_speed_mps*1000.0f),(int)(t->extent_m*1000.0f),(int)(t->shape_variation_m*1000.0f),
                (int)(t->radial_spread_mps*1000.0f));
        }
        SAY(p,"@SCENECACHEEND page=%s\r\n",tracks?"tracks":"reflections");return 1;
    }
    if(!strcmp(cmd,"safe") || !strcmp(cmd,"disable")) {
        if(p->running) { radar_stream_cancel(&p->capture);fail(p,RADAR_DSP_E_CANCELED);return 1; }
        return 0;
    }
    if(!strncmp(cmd,"scope telemetry",15U)) {
        if(!cmd[15]) { SAY(p,"[STREAM TELEMETRY] records=%u overwritten=%u; scope telemetry START COUNT (idle, maximum256)\r\n",p->log_count,p->log_overwritten);return 1; }
        if(!idle(p) || sscanf(cmd+15U," %u %u %c",&first,&count,&extra)!=2 || !count || count>256U ||
           first>=p->log_count || count>p->log_count-first ||
           (p->log_count>capacity(p) && first<p->log_count-capacity(p))) {
            SAY(p,"[STREAM TELEMETRY BLOCK] Idle valid retained range required\r\n");return 1;
        }
        SAY(p,"@STREAMBEGIN first=%u count=%u records=%u overwritten=%u epoch=%u\r\n",first,count,p->log_count,p->log_overwritten,p->time.epoch);
        p->export_index=first;p->export_remaining=count;p->exporting=1U;return 1;
    }
    if(p->running || p->exporting || p->record_exporting) {
        if(!strcmp(cmd,"status") || !strcmp(cmd,"capture status") || !strcmp(cmd,"display status") ||
           !strcmp(cmd,"display diag"))return 0;
        SAY(p,"[STREAM BLOCK] Active ownership; scope stop before configuration/export\r\n");return 1;
    }
    if(!strcmp(cmd,"capture clear")) {
        RadarStreamFrame frame;unsigned discarded=0U;
        if(!idle(p) || s->session->state!=RADAR_SESSION_IDLE) {
            SAY(p,"[STREAM BLOCK] Explicit capture recover must finish before clearing failure evidence\r\n");return 1;
        }
        while(!radar_stream_acquire(&p->capture,&frame)) {
            if(radar_stream_release(&p->capture,&frame))break;
            ++discarded;
        }
        rc=radar_stream_clear_fault(&p->capture);
        if(rc)SAY(p,"[STREAM BLOCK] Retained owner/time epoch cannot be cleared rc=%d\r\n",rc);
        else SAY(p,"[STREAM CLEARED] Explicitly discarded %u pending RAW frames; timing records and published view retained\r\n",discarded);
        return 1;
    }
    if(!strcmp(cmd,"scope scene reset")) {
        if(!idle(p) || radar_scene_record_holds(&p->recorder) || p->installation_epoch==UINT32_MAX) {
            SAY(p,"[SCENE BLOCK] Reset requires idle valid installation epoch\r\n");return 1;
        }
        ++p->installation_epoch;p->have_calibration=0U;
        radar_scene_reset(&p->scene,RADAR_SCENE_RESET_INSTALLATION);
        memset(&p->scene_frame,0,sizeof p->scene_frame);visible(p,"MAP RESET");
        SAY(p,"[SCENE RESET] Installation=%u, tracks/map cleared, calibration=UNCALIBRATED\r\n",p->installation_epoch);return 1;
    }
    if(!strcmp(cmd,"scope clear")) {
        if(!idle(p) || radar_scene_record_holds(&p->recorder) || p->capture.raw.used || p->capture.metadata_open) {
            SAY(p,"[STREAM BLOCK] Complete recovery and capture clear before clearing the view\r\n");return 1;
        }
        radar_scene_reset(&p->scene,RADAR_SCENE_RESET_CONTEXT);
        memset(&p->scene_frame,0,sizeof p->scene_frame);
        return 0; /* Existing finite display/history clear remains authoritative. */
    }
    if(!strncmp(cmd,"capture stream ",15U)) {
        uint32_t frames;
        if(!strcmp(cmd+15U,"16"))frames=16U;
        else if(!strcmp(cmd+15U,"64"))frames=64U;
        else if(!strcmp(cmd+15U,"continuous"))frames=0U;
        else { SAY(p,"[STREAM] capture stream 16|64|continuous\r\n");return 1; }
        if(select_profile(p,frames))SAY(p,"[STREAM BLOCK] Profile selection needs idle empty ownership\r\n");
        return 1;
    }
    if(!strcmp(cmd,"scope live") || !strcmp(cmd,"scope start")) {
        rc=start(p);if(rc)SAY(p,"[STREAM REFUSED] rc=%d; PL_BASELINE, ABI, confirmed stream profile and native preflight required\r\n",rc);
        return 1;
    }
    if(!strncmp(cmd,"scope start ",12U) || !strcmp(cmd,"scope show") || !strncmp(cmd,"capture batch ",14U) ||
       !strcmp(cmd,"capture once") || !strcmp(cmd,"capture export") || !strncmp(cmd,"scope pipeline ",15U)) {
        SAY(p,"[STREAM] Select capture stream 16|64|continuous, confirm, then scope live. FINITE remains a separate mode.\r\n");return 1;
    }
    return 0;
}
int radar_stream_scan_set_calibration(RadarStreamScan *p,const RadarArrayCalibration *cal,uint32_t installation_epoch)
{
    if(!p || !idle(p) || radar_scene_record_holds(&p->recorder) || !installation_epoch || (cal &&
        (strcmp(cal->array_id,RADAR_SPATIAL_ARRAY_ID) || strcmp(cal->cfg_sha256,p->scan->session->profile->cfg_sha256))))return -1;
    p->installation_epoch=installation_epoch;p->have_calibration=cal!=NULL;
    if(cal)p->calibration=*cal;
    radar_scene_reset(&p->scene,1U);return 0;
}
