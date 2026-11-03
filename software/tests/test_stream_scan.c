/* SPDX-License-Identifier: MIT
 * Production stream coordinator, RAW leases, range pack/import, spatial/scene
 * processing and DSP driver. Only acquisition source/PL registers are modeled.
 * No synthetic input enters target configuration or physical demonstration.
 */
#define _GNU_SOURCE
#include "radar_stream_scan.h"
#include "radar_crc32.h"
#include "radar_hw.h"
#include "radar_memory.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/mman.h>
#include <math.h>
static RadarScan scan;
static RadarStreamScan pipeline;
static RadarSession session;
static RadarSw2 sw2;
static RadarSw3 sw3;
static RadarControl control;
static RadarConsole console;
static uint64_t clock_us,finish_at;
static uint32_t reg[128],submitted,source_count,active_crc,cancel_stuck;
static unsigned prepare_overlap,post_overlap;
static uint8_t template_frame[RADAR_STREAM_FRAME_BYTES];
static void *scene_memory;
static RadarSpatialWeights *weights;
static RadarStreamRecord *records;
static char serial_log[128U*1024U];
static size_t serial_bytes;
static unsigned serial_calls,chunk_calls,max_format_bytes;
static void serial_clear(void){serial_bytes=0;serial_log[0]=0;serial_calls=chunk_calls=max_format_bytes=0;}
static uint32_t read_reg(void *ctx,uint32_t addr)
{
    (void)ctx;
    if(addr==12U && reg[3]==1U && !cancel_stuck && clock_us>=finish_at) {
        const RadarDspJob *j=&scan.dsp.job;
        assert(active_crc==(radar_crc32_update(UINT32_MAX,(const void *)(uintptr_t)j->input,j->input_bytes)^UINT32_MAX));
        reg[3]=2U;reg[0x78/4]=3456;reg[0x68/4]=100000;
        memset((void *)(uintptr_t)j->cube_out,0,RADAR_DSP_COMPLEX_BYTES);
        memset((void *)(uintptr_t)j->rd_out,0,RADAR_DSP_RD_BYTES);
        memset((void *)(uintptr_t)j->spectrum_out,0,RADAR_DSP_SPECTRUM_BYTES);
        memset((void *)(uintptr_t)j->candidates_out,0,RADAR_DSP_CANDIDATES_BYTES);
    }
    assert(addr<sizeof(reg));return reg[addr/4];
}
static void write_reg(void *ctx,uint32_t addr,uint32_t value)
{
    (void)ctx;assert(addr<sizeof(reg));
    if(addr==8U) {
        if(value==4U)reg[3]=0;
        else if(value==1U){reg[3]=1U;finish_at=clock_us+5000;++submitted;
            active_crc=radar_crc32_update(UINT32_MAX,(const void *)(uintptr_t)scan.dsp.job.input,scan.dsp.job.input_bytes)^UINT32_MAX;}
        else if(value==2U){if(!cancel_stuck){reg[3]=6U;reg[4]=1U;}}
    } else reg[addr/4]=value;
}
static void cache(void *ctx,uint32_t a,uint32_t n)
{(void)ctx;assert(a>=RADAR_DSP_WORK_BASE && (uint64_t)a+n<=RADAR_DSP_WORK_END);}
static void barrier(void *ctx){(void)ctx;}
static uint64_t now_cb(void *ctx){(void)ctx;return clock_us;}
void radar_dsp_platform_init(RadarDspDriver *d)
{RadarDspIo io={NULL,read_reg,write_reg,cache,cache,barrier,now_cb,1};radar_dsp_init(d,&io);}
u64 radar_hw_now_us(void){return ++clock_us;}
UINTPTR radar_memory_capture_base(void){return 0xc00000U;}
int radar_console_printf(RadarConsole *c,const char *fmt,...)
{
    char formatted[512];va_list args;int n;(void)c;
    va_start(args,fmt);n=vsnprintf(formatted,sizeof(formatted),fmt,args);va_end(args);
    /* Match the production formatter bound. Any formerly hidden long line
     * is a test failure instead of being silently accepted by the seam. */
    assert(n>=0 && (size_t)n<sizeof(formatted));++serial_calls;
    if(!strcmp(fmt,"%.*s")){assert(n<=400);++chunk_calls;}
    if((unsigned)n>max_format_bytes)max_format_bytes=(unsigned)n;
    assert(serial_bytes+(size_t)n+1U<sizeof(serial_log));
    memcpy(serial_log+serial_bytes,formatted,(size_t)n);serial_bytes+=(size_t)n;serial_log[serial_bytes]=0;
    return 0;
}
u32 radar_console_free(const RadarConsole *c){(void)c;return 4096U;}
int radar_cli_busy(const RadarCli *c){(void)c;return 0;}
int radar_dma_is_quiescent(RadarDma *d){return d->state==RADAR_DMA_READY && d->reset_confirmed;}
void radar_session_invalidate_config(RadarSession *s){s->reuse_valid=0;}
void radar_stream_init(RadarStream *r,RadarSession *s,const RadarStreamIo *io)
{memset(r,0,sizeof(*r));r->session=s;r->io=*io;radar_raw_slots_init(&r->raw,radar_memory_capture_base());}
int radar_stream_active(const RadarStream *r){return r->active;}
int radar_stream_start(RadarStream *r,u32 limit){r->active=1;r->frame_limit=limit;r->session->owns=1;
 r->session->source=RADAR_SOURCE_RUNNING;r->session->sw3->dma.state=RADAR_DMA_RUNNING;return 0;}
int radar_stream_request_stop(RadarStream *r,int reason){r->stop_requested=1;r->error=reason;r->active=0;
 r->session->owns=0;r->session->source=RADAR_SOURCE_STOPPED;r->session->state=RADAR_SESSION_IDLE;
 r->session->sw3->dma.state=RADAR_DMA_READY;r->session->sw3->dma.reset_confirmed=1;return 0;}
void radar_stream_cancel(RadarStream *r){r->active=0;r->error=RADAR_SESSION_E_CANCELED;}
int radar_stream_acquire(RadarStream *r,RadarStreamFrame *f){return radar_raw_slots_acquire(&r->raw,f);}
int radar_stream_release(RadarStream *r,const RadarStreamFrame *f){return radar_raw_slots_release(&r->raw,f);}
int radar_stream_clear_fault(RadarStream *r){r->error=0;return 0;}
static void make_frame(void)
{
    unsigned c,i;uint8_t h[64]={0};const unsigned order[4]={0,2,1,3};
    h[0]=0xdc;h[1]=0x0a;h[2]=0xda;h[3]=0x0c;memcpy(h+4,h,4);h[8]=0x30;h[10]=1;
    h[16]=0x44;h[17]=7;h[18]=32;h[20]=4;h[21]=2;h[23]=2;h[24]=15;h[25]=1;h[26]=1;h[29]=2;memset(h+52,15,12);
    for(c=0;c<96;++c)for(i=0;i<32;++i)memcpy(template_frame+c*4160+2*((i&~3U)+order[i&3U]),h+2*i,2);
}
static void setup(void)
{
    RadarDspIo io={NULL,read_reg,write_reg,cache,cache,barrier,now_cb,1};
    memset(&scan,0,sizeof(scan));memset(&session,0,sizeof(session));memset(&sw2,0,sizeof(sw2));
    memset(&sw3,0,sizeof(sw3));memset(&control,0,sizeof(control));memset(reg,0,sizeof(reg));
    memset((void *)(uintptr_t)RADAR_DSP_WORK_BASE,0,RADAR_DSP_WORK_END-RADAR_DSP_WORK_BASE);
    clock_us=100;submitted=source_count=cancel_stuck=0;prepare_overlap=post_overlap=0;
    serial_bytes=0;serial_log[0]=0;serial_calls=chunk_calls=max_format_bytes=0;
    memset(&console,0,sizeof(console));
    session.sw2=&sw2;session.sw3=&sw3;session.control=&control;session.console=&console;
    session.profile=radar_capture_profile_stream(64);session.active_session=1;
    scan.session=&session;scan.backend=PL_BASELINE;scan.dsp_config_epoch=1;
    radar_spatial_init(&scan.spatial_processor);radar_dsp_init(&scan.dsp,&io);
    reg[0]=RADAR_DSP_MAGIC;reg[1]=RADAR_DSP_ABI;
    assert(!radar_stream_scan_init(&pipeline,&scan,records,scene_memory,radar_scene_workspace_bytes(),weights));
    pipeline.mode=pipeline.running=pipeline.capture.active=1;pipeline.frame_limit=64;
    scan.active=scan.live_mode=1;
}
static void inject(int bad_crc)
{
    RadarStreamFrame *f;
    assert(!radar_raw_slots_reserve(&pipeline.capture.raw,1,source_count+1,1,clock_us,&f));
    memcpy((void *)f->raw,template_frame,sizeof(template_frame));
    f->crc32=radar_crc32_update(UINT32_MAX,(const void *)f->raw,f->bytes)^UINT32_MAX;
    f->crc32^=bad_crc?1U:0U;f->verified_us=clock_us;f->first_accept_us=(source_count+1U)*100000U;
    f->time_error_us=10;f->transport.valid=1;f->transport.sequence=source_count+1U;f->transport.bytes=f->bytes;
    assert(!radar_raw_slots_publish(&pipeline.capture.raw,f));++source_count;++pipeline.capture.frames_verified;
}
static void tick(void)
{
    unsigned i;clock_us+=100;radar_stream_scan_poll(&pipeline);
    for(i=0;i<2;++i)if(pipeline.engine_slot>=0 && (int)i!=pipeline.engine_slot && pipeline.task[i].range.running==1)
        ++prepare_overlap;
    if(pipeline.engine_slot>=0 && pipeline.post_slot>=0 && pipeline.engine_slot!=pipeline.post_slot)++post_overlap;
}
static void check_fields(const char *line,const char *const *fields,size_t count)
{
    size_t i;
    for(i=0;i<count;++i){const char *field=strstr(line,fields[i]);assert(field && field[strlen(fields[i])]);}
}
static void verify_final_ack_and_export(void)
{
    RadarTimingRecord *last=&records[63].timing;RadarTimingAck ack={0};
    const uint32_t revision=last->published_revision;unsigned i,lines=0;
    const char *at;char line[1800];const char *output=getenv("RADAR_STREAM_TEST_UART_LOG");
    static const char *const record_fields[]={
        "record="," frame="," epoch="," seq="," crc="," flags="," stages="," raw="," lease=",
        " task="," tasklease="," first="," last="," mapped="," uncertainty="," observed="," ready=",
        " front="," submitted="," done="," published="," ack="," cpu="," render="," queue="," high=",
        " revision="," page="," error="," bytes="," hz="," tstatus="," ackseq="," ackrevision=",
        " ackframe="," renderbegin="," renderend="," queued="," coalesced="};
    static const char *const status_fields[]={
        "running="," limit="," epoch="," verified="," processed="," presented="," ready="," owners=",
        " high="," error="," records="," overwritten="," time_valid="," uncertainty_us=",
        " skipped_display="," pending_display="," crc_errors="," hsi_errors="," dma_errors=",
        " metadata_errors="," dp_errors="," dp_active="," health_cpu_us="," health_max_us="};
    assert(!pipeline.running && pipeline.completed==64 && pipeline.presented==0);
    assert(pipeline.stop_view_pending && revision && scan.revision==revision);
    assert(!(last->stages&RADAR_TIMING_ACKED));
    for(i=0;i<16;++i)tick();
    assert(scan.revision==revision && pipeline.stop_view_pending && pipeline.presented==0);
    ack.sequence=1;ack.frame_id=64;ack.data_revision=revision+1U;
    radar_stream_scan_ack(&pipeline,&ack);tick();
    assert(pipeline.presented==0 && scan.revision==revision && pipeline.stop_view_pending);
    ack.sequence=2;ack.data_revision=revision;ack.view_revision=7;ack.page=2;ack.coalesced=63;
    ack.render_begin_ps_us=clock_us+10;ack.render_end_ps_us=clock_us+20;
    ack.queue_ps_us=clock_us+25;ack.ack_ps_us=clock_us+40;clock_us+=40;
    radar_stream_scan_ack(&pipeline,&ack);
    assert(pipeline.presented==1 && last->first_ack.sequence==2 && (last->stages&RADAR_TIMING_ACKED));
    assert(scan.revision==revision && pipeline.stop_view_pending);
    tick();assert(!pipeline.stop_view_pending && scan.revision==revision+1U);
    assert(!strcmp(scan.status,"LIVE STOPPED") && last->published_revision==revision);
    radar_stream_scan_ack(&pipeline,&ack);tick();
    assert(pipeline.presented==1 && scan.revision==revision+1U && last->first_ack.ack_ps_us==ack.ack_ps_us);
    puts("PASS final frame64 revision survives16 idle polls and mismatchedACK; exact lateACK counts once, then status revision advances once");

    serial_bytes=0;serial_log[0]=0;serial_calls=chunk_calls=max_format_bytes=0;
    assert(radar_stream_scan_command(&pipeline,"scope status")==1);
    assert(radar_stream_scan_command(&pipeline,"scope telemetry 0 64")==1 && pipeline.exporting);
    for(i=0;pipeline.exporting;++i){assert(i<66);tick();}
    assert(!console.dropped && max_format_bytes<512U && chunk_calls>=128U);
    assert(strstr(serial_log,"@STREAMBEGIN first=0 count=64 records=64 overwritten=0 epoch=0\r\n"));
    assert(strstr(serial_log,"@STREAMEND next=64 records=64 epoch=0\r\n"));
    at=strstr(serial_log,"[STREAM STATUS]");assert(at);
    {const char *end=strstr(at,"\r\n");size_t bytes;assert(end);bytes=(size_t)(end-at);assert(bytes<sizeof(line));
        memcpy(line,at,bytes);line[bytes]=0;check_fields(line,status_fields,sizeof(status_fields)/sizeof(status_fields[0]));
        assert(strstr(line," processed=64 ") && strstr(line," presented=1 ") && strstr(line," skipped_display=63 ") && strstr(line," pending_display=0 "));}
    at=serial_log;
    while((at=strstr(at,"@STREAM record="))!=NULL){
        const char *end=strstr(at,"\r\n");size_t bytes;unsigned record_id,frame_id;assert(end);bytes=(size_t)(end-at);
        assert(bytes<sizeof(line));memcpy(line,at,bytes);line[bytes]=0;
        check_fields(line,record_fields,sizeof(record_fields)/sizeof(record_fields[0]));
        assert(sscanf(line,"@STREAM record=%u frame=%u",&record_id,&frame_id)==2);
        assert(record_id==lines+1U && frame_id==lines+1U);
        if(frame_id==64U){char expected[64];snprintf(expected,sizeof(expected)," ackrevision=%u ",revision);
            assert(strstr(line,expected) && strstr(line," ackseq=2 ") && strstr(line," ackframe=64 ") && strstr(line," coalesced=63"));}
        else assert(strstr(line," ackseq=0 ") && strstr(line," ackframe=0 "));
        ++lines;at=end+2;
    }
    assert(lines==64U);
    if(output){FILE *f=fopen(output,"wb");assert(f);assert(fwrite(serial_log,1,serial_bytes,f)==serial_bytes);assert(!fclose(f));}
    printf("PASS complete STREAM export: BEGIN/END,64 records and status fields; %u formatter calls, %u chunks, maximum%u bytes per512-byte formatter\n",
        serial_calls,chunk_calls,max_format_bytes);
}
static void record_setup(void)
{
    setup();pipeline.running=pipeline.capture.active=scan.active=0;pipeline.frame_limit=0;
    session.profile=radar_capture_profile_stream(0);session.confirmed=1;pipeline.dp_active=1;
    session.source=RADAR_SOURCE_STOPPED;session.state=RADAR_SESSION_IDLE;
    sw3.dma.state=RADAR_DMA_READY;sw3.dma.reset_confirmed=1;
    /* Earlier ordinary telemetry may have wrapped. Arming replaces its index,
     * not the16MiB processing reservation or any pending ACK owner. */
    pipeline.log_count=9000;pipeline.log_overwritten=808;
    assert(radar_stream_scan_command(&pipeline,"scope record arm 60"));
    assert(pipeline.recorder.state==RADAR_RECORD_ARMED && pipeline.log_capacity==2048 && !pipeline.log_count);
}
static void record_start(void)
{
    pipeline.dp_active=0;assert(radar_stream_scan_command(&pipeline,"scope live"));
    assert(!pipeline.running && pipeline.recorder.state==RADAR_RECORD_ARMED);
    pipeline.dp_active=1;assert(radar_stream_scan_command(&pipeline,"scope live"));
    assert(pipeline.running && pipeline.recorder.state==RADAR_RECORD_ACTIVE);
}
static void ack_last(void)
{
    RadarTimingAck a={0};RadarTimingRecord *r=&records[(pipeline.log_count-1U)%pipeline.log_capacity].timing;
    a.sequence=pipeline.last_ack+1;a.frame_id=r->frame_id;a.data_revision=r->published_revision;
    a.ack_ps_us=clock_us;a.queue_ps_us=clock_us-10;a.render_begin_ps_us=clock_us-30;
    radar_stream_scan_ack(&pipeline,&a);
}
static void assert_not_hashing(void){tick();assert(pipeline.recorder.state==RADAR_RECORD_DRAINING);}
static void verify_record_gates(void)
{
    unsigned i;uint32_t before;RadarTimingAck late={0};uint8_t header[4096];
    record_setup();record_start();inject(0);
    for(i=0;pipeline.completed<1;++i){assert(i<10000);tick();assert(!pipeline.error);}
    assert(pipeline.recorder.count==1 && !pipeline.presented);
    assert(pipeline.scene_frame.diagnostics.processing_us>0);
    {uint32_t measured;memcpy(&measured,(uint8_t *)records+RADAR_RECORD_FRAMES_OFFSET+2512,4);
        assert(measured==pipeline.scene_frame.diagnostics.processing_us);}
    assert(radar_stream_scan_command(&pipeline,"scope stop"));
    for(i=0;pipeline.running;++i){assert(i<1000);tick();}
    assert(pipeline.stop_view_pending);assert_not_hashing();
    assert(radar_stream_scan_command(&pipeline,"scope record clear"));assert(pipeline.log_capacity==2048);
    ack_last(); /* Ownership, independently of ACK, must still gate freeze. */
    session.owns=1;assert_not_hashing();session.owns=0;
    pipeline.capture.raw.used=1;assert_not_hashing();pipeline.capture.raw.used=0;
    pipeline.capture.metadata_open=1;assert_not_hashing();pipeline.capture.metadata_open=0;
    sw3.dma.lease_active=1;assert_not_hashing();sw3.dma.lease_active=0;
    sw3.dma.packet_queued=1;assert_not_hashing();sw3.dma.packet_queued=0;
    sw3.dma.reset_confirmed=0;assert_not_hashing();sw3.dma.reset_confirmed=1;
    sw3.dma.state=RADAR_DMA_RUNNING;assert_not_hashing();sw3.dma.state=RADAR_DMA_READY;
    session.source=RADAR_SOURCE_RUNNING;assert_not_hashing();session.source=RADAR_SOURCE_STOPPED;
    pipeline.task[0].state=1;assert_not_hashing();pipeline.task[0].state=0;
    scan.dsp.state=RADAR_DSP_LOCKED;assert_not_hashing();scan.dsp.state=RADAR_DSP_IDLE;
    tick();assert(pipeline.recorder.state==RADAR_RECORD_HASHING);
    before=radar_crc32_update(UINT32_MAX,records,320);
    late.sequence=pipeline.last_ack+100;late.frame_id=1;late.data_revision=records[0].timing.published_revision;
    radar_stream_scan_ack(&pipeline,&late);assert(before==radar_crc32_update(UINT32_MAX,records,320));
    for(i=0;pipeline.recorder.state!=RADAR_RECORD_FROZEN;++i){assert(i<100);tick();}
    assert(pipeline.recorder.incomplete && pipeline.recorder.count==1 && pipeline.presented==1);
    assert(!radar_scene_record_read(&pipeline.recorder,0,header,sizeof header));
    radar_stream_scan_ack(&pipeline,&late);assert(before==radar_crc32_update(UINT32_MAX,records,320));
    assert(radar_stream_scan_command(&pipeline,"scope live"));assert(!pipeline.running);
    serial_clear();assert(radar_stream_scan_command(&pipeline,"scope record export 999999 4096"));assert(!pipeline.record_exporting);
    assert(radar_stream_scan_command(&pipeline,"scope record export 0 4096"));assert(pipeline.record_exporting);
    assert(radar_stream_scan_command(&pipeline,"scope telemetry 0 1"));assert(!pipeline.exporting);
    for(i=0;pipeline.record_exporting;++i){assert(i<34);tick();}
    assert(strstr(serial_log,"@SCENEBEGIN schema=1") && strstr(serial_log,"@SCENEDATA epoch=1 offset=0 bytes=128 crc=") &&
        strstr(serial_log,"@SCENEEND epoch=1 next=4096 bytes="));
    assert(max_format_bytes<512 && !console.dropped);
    assert(radar_stream_scan_command(&pipeline,"scope record clear"));assert(pipeline.log_capacity==8192 && !pipeline.log_count);
    pipeline.scene_frame.reflection_count=8;serial_clear();
    for(i=0;i<8;++i){RadarStableReflection *v=&pipeline.scene_frame.reflections[i];
        memset(v,0,sizeof *v);v->id=i+1;v->range_m=v->range_median_m=2.0f;v->u=NAN;v->power_dbfs=-50;v->snr_db=20;}
    assert(radar_stream_scan_command(&pipeline,"scope scene"));
    assert(strstr(serial_log,"@SCENEDIAG reject=") && strstr(serial_log,"@REFLECTION id=8 ") &&
        strstr(serial_log,"u_milli=UNAVAILABLE") && strstr(serial_log,"@SCENECACHEEND page=reflections"));
    assert(serial_bytes<3800U && !console.dropped);
    puts("PASS RECORD actual final ACK plus each source/RAW/DSP/metadata/DMA owner gates freeze; HASHING/FROZEN late ACK immutable; bounds, console ownership,8192 restore");
    record_setup();record_start();inject(0);
    for(i=0;pipeline.completed<1;++i){assert(i<10000);tick();}
    assert(!pipeline.presented);assert(radar_stream_scan_command(&pipeline,"safe"));
    for(i=0;pipeline.running;++i){assert(i<1000);tick();}
    assert(pipeline.error && pipeline.recorder.state!=RADAR_RECORD_FROZEN);
    /* Explicit recovery seam: no fabricated display ACK, all actual owners
     * must be quiet before evidence can be intentionally abandoned. */
    assert(radar_stream_scan_command(&pipeline,"scope record discard"));
    assert(pipeline.recorder.state==RADAR_RECORD_EMPTY && pipeline.log_capacity==8192 && !pipeline.presented);
    puts("PASS published but unACKed frame cancellation can recover and explicitly discard; never certifies FROZEN or presentation");
}
static void verify_record_duration(void)
{
    unsigned polls=0;const char *file=getenv("RADAR_STREAM_SCENE_FIXTURE");
    record_setup();record_start();
    while(pipeline.running){
        assert(++polls<1000000);
        if(pipeline.capture.active && pipeline.capture.raw.used<2){assert(source_count<605);inject(0);}
        tick();assert(!pipeline.error);
    }
    assert(pipeline.completed>=601 && pipeline.recorder.count==pipeline.completed);
    assert(pipeline.recorder.heat_count==3 && pipeline.recorder.stop_due && !pipeline.presented);
    assert(pipeline.recorder.state==RADAR_RECORD_DRAINING && pipeline.stop_view_pending);
    assert(pipeline.recorder.last_us-pipeline.recorder.first_us>=60000000U);
    ack_last();for(polls=0;pipeline.recorder.state!=RADAR_RECORD_FROZEN;++polls){assert(polls<1500);tick();}
    assert(!pipeline.recorder.incomplete && !pipeline.recorder.error && !pipeline.capture.raw.used);
    if(file){FILE *f=fopen(file,"wb");uint8_t block[4096];uint32_t at=0;assert(f);
        while(at<pipeline.recorder.image_bytes){uint32_t n=pipeline.recorder.image_bytes-at;if(n>sizeof block)n=sizeof block;
            assert(!radar_scene_record_read(&pipeline.recorder,at,block,n));assert(fwrite(block,1,n,f)==n);at+=n;}
        assert(!fclose(f));}
    printf("PASS production coordinator auto60s boundary stop: %u processed,3 sliced heatmaps, no ACK acquisition gate, final actual ACK then complete freeze\n",pipeline.completed);
}
int main(void)
{
    unsigned polls=0,i;
    void *raw=mmap((void *)0xc00000U,0x800000U,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
    void *dsp=mmap((void *)(uintptr_t)RADAR_DSP_WORK_BASE,RADAR_DSP_WORK_END-RADAR_DSP_WORK_BASE,
        PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
    assert(raw==(void *)0xc00000U && dsp==(void *)(uintptr_t)RADAR_DSP_WORK_BASE);
    scene_memory=malloc(radar_scene_workspace_bytes());weights=malloc(sizeof(*weights));
    records=calloc(1,RADAR_RECORD_REGION_BYTES);assert(scene_memory&&weights&&records);
    make_frame();setup();inject(0);
    while(pipeline.running){
        assert(++polls<100000);
        if(source_count<64 && pipeline.capture.raw.used<2 && (source_count!=1 || pipeline.engine_slot>=0))inject(0);
        if(source_count==64)pipeline.capture.active=0;
        tick();assert(!pipeline.error);
    }
    assert(pipeline.completed==64 && submitted==64 && !pipeline.capture.raw.used);
    assert(prepare_overlap && post_overlap && pipeline.presented==0); /* no display ACK supplied */
    for(i=0;i<64;++i){assert(records[i].timing.frame_id==i+1U);assert(records[i].timing.stages&RADAR_TIMING_PUBLISHED);}
    puts("PASS production STREAM coordinator64frames: dualslot prepare/engine/post overlap, immutable input, RAW reuse, no display ACK gate");
    verify_final_ack_and_export();
    setup();inject(1);for(i=0;i<1000 && pipeline.running;++i)tick();
    assert(pipeline.error==-603 && !pipeline.completed && !submitted && !pipeline.capture.raw.used);
    puts("PASS bad copied CRC is refused before PL submission/publication");
    setup();inject(0);for(i=0;i<1000 && pipeline.engine_slot<0;++i)tick();assert(pipeline.engine_slot>=0);
    cancel_stuck=1;inject(1);for(i=0;i<3000;++i)tick();
    assert(pipeline.error==-603 && scan.dsp.state==RADAR_DSP_LOCKED && !pipeline.completed);
    assert(pipeline.capture.raw.processing==2 && pipeline.running);
    puts("PASS CRC failure during other slot PL work: canceled/LOCKED engine retains both owners and publishes nothing");
    setup();pipeline.running=pipeline.capture.active=scan.active=0;pipeline.capture.metadata_open=1;
    assert(radar_stream_scan_command(&pipeline,"capture mode finite")==1 && pipeline.mode==1);
    assert(radar_stream_scan_command(&pipeline,"capture stream 16")==1 && pipeline.frame_limit==64);
    puts("PASS failed time-epoch ownership blocks both mode and profile changes until explicit clear");
    verify_record_gates();
    verify_record_duration();
    free(records);free(weights);free(scene_memory);return 0;
}
