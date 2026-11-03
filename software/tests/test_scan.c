/* SPDX-License-Identifier: MIT
 * Scheduler contract tests, not hardware/DSP validation. Native services and
 * the one-chirp DSP API are deterministic stubs; production CRC is linked.
 */
#define _GNU_SOURCE
#include "radar_scan.h"
#include "radar_crc32.h"
#include "radar_hw.h"
#include "radar_memory.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

static RadarScan scan;
static RadarSession session;
static RadarSw2 sw2;
static RadarSw3 sw3;
static RadarControl control;
static RadarConsole console;
static RadarCaptureProfile profile;
static u8 raw[RADAR_RANGE_INPUT_BYTES];
static u8 batch_raw[16U*RADAR_RANGE_INPUT_BYTES];
static u8 *capture_store;
static u64 now;
static u32 hz,gate,starts,clears,qualifies,connects,queries,safes,cancels;
static u32 console_room,data_headers,data_rows,data_footers;
static u32 timing_headers,timing_footers,timing_blocks;
static u32 config_invalidations;
static int qualify_ready,profile_bad,dsp_bad,rate_bad;
static int spatial_bad,spatial_sink_bad,spatial_begin_bad;
static unsigned spatial_polls;
static unsigned spatial_done_after,spatial_fail_at,profile_validations;
static u64 spatial_step_us,profile_validation_us,range_step_us;
static u32 pl_regs[80],pl_enabled,pl_pack_calls,pl_power_imports,pl_rows,ps_range_calls,pl_flags,pl_bad_header;
static uint32_t pl_read(void *context,uint32_t offset)
{ (void)context;return pl_regs[offset/4U]; }
static void pl_write(void *context,uint32_t offset,uint32_t value)
{
    (void)context;
    if(offset==8U) {
        if(value==4U)pl_regs[3]=0U;
        if(value==1U) {
            pl_regs[3]=2U;pl_regs[0x78/4]=3456U;pl_regs[0x7c/4]=pl_flags;
            pl_regs[0x68/4]=9876543U;pl_regs[0x6c/4]=1U;
            pl_regs[0x70/4]=pl_bad_header;
        }
    } else pl_regs[offset/4U]=value;
}
static void pl_cache(void *context,uint32_t addr,uint32_t bytes)
{ (void)context;assert(addr>=RADAR_DSP_WORK_BASE&&(u64)addr+bytes<=RADAR_DSP_WORK_END); }
static void pl_barrier(void *context) { (void)context; }
static uint64_t pl_now(void *context) { (void)context;return now; }

u64 radar_hw_now_us(void) { return now; }
UINTPTR radar_memory_capture_base(void) { return (UINTPTR)capture_store; }
int radar_console_printf(RadarConsole *c,const char *fmt,...)
{
    char line[512];va_list args;
    (void)c;va_start(args,fmt);vsnprintf(line,sizeof(line),fmt,args);va_end(args);
    if(!strncmp(line,"@SCOPEBEGIN",11U)) ++data_headers;
    if(!strncmp(line,"@SCOPEPOWER",11U)) ++data_rows;
    if(!strncmp(line,"@SCOPEEND",9U)) ++data_footers;
    if(!strncmp(line,"@TIMING id=",11U))++timing_headers;
    if(!strncmp(line,"@TIMINGEND ",11U))++timing_footers;
    if(!strncmp(line,"[TIMING BLOCK]",14U))++timing_blocks;
    if(!strncmp(line,"@TIMING",7U))assert(strlen(line)<sizeof line-1U);
    if(!strncmp(line,"[SAMPLE FRESHNESS]",18U)) {
        assert(strstr(line,"scope=SAMPLED_ONLY")&&strstr(line,"current_eligible=0"));
        assert(strlen(line)<sizeof line-1U);
    }
    return 0;
}
u32 radar_console_free(const RadarConsole *c) { (void)c;return console_room; }
int radar_capture_profile_validate(const RadarCaptureProfile *p)
{ (void)p;++profile_validations;now+=profile_validation_us;return profile_bad; }
int radar_capture_profile_supported(const RadarCaptureProfile *p)
{ return p && !strcmp(p->profile_id,RADAR_SCAN_PROFILE_ID) && !strcmp(p->cfg_sha256,RADAR_RANGE_CFG_SHA256); }
void radar_dsp_platform_init(RadarDspDriver *d)
{ RadarDspIo io={NULL,pl_read,pl_write,pl_cache,pl_cache,pl_barrier,pl_now,0};io.mapped=(int)pl_enabled;radar_dsp_init(d,&io); }
int radar_range_pack_step(RadarRangeProcessor *p,int32_t *iq,size_t words)
{ (void)iq;assert(words==196608U);++pl_pack_calls;++p->next_chirp;
  if(p->next_chirp<96U)return 0;
  p->running=2U;return 1; }
int radar_range_import_power(RadarRangeProcessor *p,const uint64_t power[256])
{ (void)power;++pl_power_imports;p->running=0U;p->frame.valid=1U;p->frame.fft_count=384U;return 1; }
int radar_spatial_import_pl_begin(RadarSpatialProcessor *p,const RadarDspCandidateHeader *h,const RadarDspCandidate *c,RadarDspBackend b)
{ (void)p;(void)c;assert(h->count==0U&&(b==PL_BASELINE||b==PL_ENHANCED));return 0; }
int radar_spatial_import_pl_row(RadarSpatialProcessor *p,unsigned r,const int32_t *cube,const uint64_t *power)
{ (void)p;(void)cube;(void)power;++pl_rows;return r==255U?1:0; }
int radar_capture_profile_version_matches(const RadarCaptureProfile *p,const char *v)
{ (void)p;return !strcmp(v,"SDK 03.05.00.04"); }
int radar_cli_busy(const RadarCli *c)
{ return c->state==RADAR_CLI_RESPONSE; }
int radar_control_read32(RadarControl *c,u8 page,u32 *v)
{ (void)c;assert(page==RADAR_PAGE_SYS_HZ);*v=hz;return rate_bad; }
u32 radar_session_preflight(RadarSession *c)
{
    return gate || c->owns || c->exporting || c->result.valid ||
        !c->confirmed || !qualify_ready || sw2.state!=SW2_ONLINE ||
        !sw2.cli_verified || !sw2.check10_passes || !sw2.reset3_passes || !c->next_session;
}
void radar_session_print_preflight(RadarSession *c) { (void)c; }
void radar_session_invalidate_config(RadarSession *c) { (void)c;++config_invalidations; }
int radar_session_start(RadarSession *c)
{
    assert(!radar_session_preflight(c));
    ++starts;c->owns=1U;c->state=RADAR_SESSION_RECEIVE;
    c->active_session=c->next_session++;
    qualify_ready=0;sw2.state=SW2_CAPTURE;
    return 0;
}
int radar_session_lease_result(RadarSession *c,RadarCaptureResult *copy)
{
    assert(c->result.valid&&!c->owns&&!c->raw_store_owner);
    *copy=c->result;c->raw_store_owner=c->result.session;
    memset(&c->result,0,sizeof c->result);c->state=RADAR_SESSION_IDLE;return 0;
}
int radar_session_release_raw(RadarSession *c,u32 owner)
{ assert(owner&&c->raw_store_owner==owner);c->raw_store_owner=0U;return 0; }
void radar_session_cancel(RadarSession *c)
{ ++cancels;c->state=RADAR_SESSION_FAILED; }
int radar_session_command(RadarSession *c,const char *cmd)
{
    assert(!strcmp(cmd,"capture clear"));
    assert(!c->owns && !c->exporting && c->result.valid);
    ++clears;memset(&c->result,0,sizeof(c->result));c->state=RADAR_SESSION_IDLE;
    return 1;
}
int radar_sw3_capture_ready(RadarSw3 *s) { (void)s;return qualify_ready; }
int radar_sw3_command(RadarSw3 *s,const char *cmd)
{
    if(!strcmp(cmd,"safe")) { s->job=SW3_IDLE;qualify_ready=0;return 0; }
    assert(!strcmp(cmd,"dma qualify"));
    assert(sw2.state==SW2_SAFE && !session.result.valid && !session.owns);
    ++qualifies;s->job=SW3_OBSERVE;s->check_error=0;qualify_ready=0;
    return 1;
}
void radar_sw2_command(RadarSw2 *s,const char *cmd)
{
    if(!strcmp(cmd,"safe")) {
        ++safes;s->state=SW2_SAFE;s->job=SW2_JOB_NONE;
        s->cli.state=RADAR_CLI_IDLE;s->cli_verified=0U;return;
    }
    if(!strcmp(cmd,"connect")) {
        assert(s->state==SW2_SAFE);++connects;s->state=SW2_CONNECT_WAIT;return;
    }
    assert(!strcmp(cmd,"query") && s->state==SW2_ONLINE);
    ++queries;s->state=SW2_QUERY;s->job=SW2_JOB_QUERY;
    s->cli.state=RADAR_CLI_RESPONSE;
}
void radar_range_init(RadarRangeProcessor *p)
{ memset(p,0,sizeof(*p));p->initialized=1U; }
int radar_range_set_fft_sink(RadarRangeProcessor *p,RadarRangeFftSink sink,void *context)
{ p->fft_sink=sink;p->fft_sink_context=context;return 0; }
void radar_spatial_init(RadarSpatialProcessor *p)
{ memset(p,0,sizeof(*p)); }
int radar_spatial_begin(RadarSpatialProcessor *p,uint32_t id,uint64_t us,const RadarArrayCalibration *c)
{ (void)c;memset(&p->frame,0,sizeof(p->frame));p->frame.session=id;p->frame.timestamp_us=us;spatial_polls=0U;return spatial_begin_bad ? RADAR_SPATIAL_E_CALIBRATION : 0; }
int radar_spatial_accept_fft(void *p,uint32_t c,uint32_t r,const float *re,const float *im)
{ (void)p;(void)c;(void)r;(void)re;(void)im;return spatial_sink_bad ? RADAR_SPATIAL_E_NUMERIC : 0; }
int radar_spatial_step(RadarSpatialProcessor *p)
{
    ++spatial_polls;now+=spatial_step_us;
    if(spatial_bad || (spatial_fail_at && spatial_polls==spatial_fail_at)) return RADAR_SPATIAL_E_NUMERIC;
    if(spatial_polls<spatial_done_after) return RADAR_SPATIAL_BUSY;
    p->frame.valid=1U;return RADAR_SPATIAL_DONE;
}
void radar_spatial_cancel(RadarSpatialProcessor *p) { p->frame.valid=0U; }
const RadarSpatialFrame *radar_spatial_result(const RadarSpatialProcessor *p)
{ return p->frame.valid ? &p->frame : NULL; }
int radar_range_begin(RadarRangeProcessor *p,const void *r,size_t bytes,u32 id,u64 us)
{
    assert(bytes==sizeof(raw) && !p->running);
    p->raw=r;p->next_chirp=0U;p->running=1U;
    memset(&p->frame,0,sizeof(p->frame));p->frame.session=id;p->frame.timestamp_us=us;
    return 0;
}
int radar_range_step(RadarRangeProcessor *p)
{
    unsigned rx;
    ++ps_range_calls;
    assert(p->running);
    now+=range_step_us;
    if(dsp_bad) return RADAR_RANGE_E_HSI;
    for(rx=0U;rx<4U;++rx)
        if(p->fft_sink && p->fft_sink(p->fft_sink_context,p->next_chirp,rx,NULL,NULL))
            return RADAR_RANGE_E_SINK;
    if(++p->next_chirp<RADAR_RANGE_CHIRPS) return 0;
    p->running=0U;p->frame.valid=1U;p->frame.peak_bin=20U;
    p->frame.peak_dbfs=-20.5f;return 1;
}
void radar_range_cancel(RadarRangeProcessor *p) { p->running=0U; }
const RadarRangeFrame *radar_range_result(const RadarRangeProcessor *p)
{ return p->frame.valid ? &p->frame : NULL; }
const char *radar_range_error_name(int e) { (void)e;return "stub DSP error"; }

static void setup(void)
{
    memset(&session,0,sizeof(session));memset(&sw2,0,sizeof(sw2));
    memset(&sw3,0,sizeof(sw3));memset(&control,0,sizeof(control));
    memset(&console,0,sizeof(console));memset(&profile,0,sizeof(profile));
    memset(raw,0x5a,sizeof(raw));
    capture_store=raw;
    pl_enabled=pl_pack_calls=pl_power_imports=pl_rows=ps_range_calls=pl_flags=pl_bad_header=0U;
    memset(pl_regs,0,sizeof pl_regs);pl_regs[0]=RADAR_DSP_MAGIC;pl_regs[1]=RADAR_DSP_ABI;
    now=1000U;hz=100000000U;gate=starts=clears=qualifies=connects=queries=safes=cancels=0U;
    qualify_ready=1;profile_bad=dsp_bad=rate_bad=0;config_invalidations=0U;
    spatial_bad=spatial_sink_bad=spatial_begin_bad=0;spatial_polls=0U;
    spatial_done_after=3U;spatial_fail_at=profile_validations=0U;
    spatial_step_us=500U;profile_validation_us=range_step_us=0U;
    console_room=8192U;data_headers=data_rows=data_footers=0U;
    profile.profile_id=RADAR_SCAN_PROFILE_ID;profile.cfg_sha256=RADAR_RANGE_CFG_SHA256;
    profile.packet_bytes=RADAR_RANGE_INPUT_BYTES;profile.packet_count=1U;
    profile.qualification=RADAR_CAPTURE_FINITE_FRAME_TEST;profile.format_flags=8U;
    profile.startup_mode=RADAR_CAPTURE_START_REVIEWED_PREAMBLE;
    session.sw2=&sw2;session.sw3=&sw3;session.control=&control;session.console=&console;
    session.profile=&profile;session.confirmed=1U;session.next_session=1U;
    control.abi_verified=1U;sw2.wiring_confirmed=1U;
    sw2.state=SW2_ONLINE;sw2.cli_verified=1U;
    sw2.check10_passes=sw2.reset3_passes=1U;
    sw2.cli.version_valid=1U;sw2.cli.version_epoch=sw2.cli.reset_epoch=3U;
    strcpy(sw2.cli.last_version,"SDK 03.05.00.04");
    radar_scan_init(&scan,&session);
}

static void poll(void) { now+=100U;radar_scan_poll(&scan); }
static void acknowledge(void) { radar_scan_presented(&scan,scan.revision);poll(); }
static void complete(u64 tick)
{
    RadarCaptureResult *r=&session.result;
    assert(session.owns);
    memset(r,0,sizeof(*r));r->valid=1U;r->session=session.active_session;
    r->bytes=r->packet_bytes=sizeof(raw);r->packet_count=1U;r->format=8U;
    r->startup_mode=profile.startup_mode;r->snapshot.first_sys_tick=tick;
    r->snapshot.complete_sys_tick=tick+198142U;
    r->complete_us=now;
    r->crc32=radar_crc32_update(UINT32_MAX,raw,sizeof(raw))^UINT32_MAX;
    session.owns=0U;session.state=RADAR_SESSION_DONE;sw2.state=SW2_SAFE;
    session.source=RADAR_SOURCE_STOPPED;
    sw2.cli_verified=0U;sw2.cli.state=RADAR_CLI_IDLE;
}
static void process(void)
{
    u32 i;
    assert(scan.state==RADAR_SCAN_PROCESS);
    for(i=0U;i<RADAR_RANGE_CHIRPS-1U;++i) { poll();assert(scan.active && scan.state==RADAR_SCAN_PROCESS); }
    poll();assert(scan.state==RADAR_SCAN_PROCESS);
    assert(scan.count==0U || scan.history[scan.count-1U].session!=scan.processing_session);
    poll();assert(scan.state==RADAR_SCAN_PROCESS);
    poll();assert(scan.state==RADAR_SCAN_WAIT_FRAME_PRESENT);
    assert(scan.spatial_frame.session==scan.history[scan.count-1U].session);
    assert(scan.spatial_frame.timestamp_us==scan.history[scan.count-1U].timestamp_us);
    assert(scan.published_session==scan.spatial_frame.session);
}
static void next_capture(void)
{
    assert(scan.state==RADAR_SCAN_WAIT_FRAME_PRESENT);
    acknowledge();assert(scan.state==RADAR_SCAN_BEGIN_QUALIFY);
    poll();assert(scan.state==RADAR_SCAN_QUALIFY && sw3.job==SW3_OBSERVE);
    sw3.job=SW3_IDLE;qualify_ready=1;poll();
    assert(scan.state==RADAR_SCAN_CONNECT && sw2.state==SW2_CONNECT_WAIT);
    sw2.state=SW2_ONLINE;poll();assert(scan.state==RADAR_SCAN_QUERY);
    sw2.state=SW2_ONLINE;sw2.job=SW2_JOB_NONE;sw2.cli.state=RADAR_CLI_IDLE;
    sw2.cli_verified=1U;poll();assert(scan.state==RADAR_SCAN_CAPTURE && session.owns);
}
static void start(const char *cmd)
{
    assert(radar_scan_command(&scan,cmd)==1 && scan.active);
    assert(starts==0U);acknowledge();assert(starts==1U && session.owns);
}

static void test_admission_and_display(void)
{
    static const char *const bad[]={"scope start 0","scope start 33","scope start -1",
        "scope start 4294967296","scope start 1x","scope startx","scope start "};
    unsigned i;
    setup();
    for(i=0U;i<sizeof(bad)/sizeof(bad[0]);++i) {
        assert(radar_scan_command(&scan,bad[i])==1 && !scan.active && !starts);
    }
    gate=1U;radar_scan_command(&scan,"scope start 2");assert(!scan.active);
    gate=0U;session.confirmed=0U;radar_scan_command(&scan,"scope start 2");assert(!scan.active);
    session.confirmed=1U;profile.cfg_sha256="wrong";
    radar_scan_command(&scan,"scope start 2");assert(!scan.active);
    profile.cfg_sha256=RADAR_RANGE_CFG_SHA256;
    radar_scan_command(&scan,"scope start");assert(scan.active && scan.batch_total==16U);
    radar_scan_presented(&scan,scan.revision-1U);poll();assert(starts==0U);
    now+=3000000U;poll();assert(!scan.active && !starts && !safes && !cancels);
    setup();radar_scan_command(&scan,"scope start 1");
    radar_scan_command(&scan,"scope stop");assert(!scan.active && !starts && safes==1U);
    setup();radar_scan_command(&scan,"scope start 1");gate=1U;acknowledge();
    assert(!scan.active && !starts); /* Recheck after screen acknowledgement. */
}

static void test_two_frames_and_retention(void)
{
    u32 revision;
    setup();start("scope start 2");complete(UINT64_C(5000000000));poll();process();
    assert(scan.count==1U && scan.history[0].timestamp_us==0U);
    assert(starts==1U && !clears && session.result.valid);
    poll();assert(starts==1U && !clears);
    next_capture();assert(starts==2U && qualifies==1U && connects==1U && queries==1U);
    assert(sw2.check10_passes==1U && sw2.reset3_passes==1U);
    complete(UINT64_C(5123456789));poll();process();acknowledge();
    assert(!scan.active && scan.count==2U && scan.batch_done==2U && session.result.valid);
    assert(scan.history[1].timestamp_us==1234567U && session.result.session==2U);
    revision=scan.revision;radar_scan_command(&scan,"scope show");
    assert(!scan.active && scan.count==2U && scan.revision!=revision && starts==2U);
    radar_scan_command(&scan,"scope start 1");assert(!scan.active && starts==2U);
    radar_scan_command(&scan,"scope clear");assert(scan.count==0U && session.result.valid);
    radar_scan_command(&scan,"scope show");process();acknowledge();
    assert(scan.count==1U && !scan.active && starts==2U && scan.history[0].timestamp_us==0U);
}

static void make_retained(void)
{ start("scope start 1");complete(UINT64_C(7000000000));poll(); }

static void test_corruption_and_commands(void)
{
    setup();make_retained();raw[sizeof(raw)-1U]^=1U;
    while(scan.active && scan.state==RADAR_SCAN_PROCESS) poll();
    assert(!scan.active && !scan.count && session.result.valid && starts==1U);
    assert(config_invalidations==1U);
    setup();make_retained();session.result.session++;
    poll();assert(!scan.active && !scan.count && session.result.valid && config_invalidations==1U);
    setup();make_retained();dsp_bad=1;
    poll();assert(!scan.active && !scan.count && config_invalidations==1U);
    setup();make_retained();
    assert(radar_scan_command(&scan,"capture clear")==1 && clears==0U);
    assert(radar_scan_command(&scan,"capture export")==1 && !session.exporting);
    assert(radar_scan_command(&scan,"capture status")==0);
    assert(radar_scan_command(&scan,"scope clear")==1 && scan.active);
    assert(radar_scan_command(&scan,"safe")==0 && !scan.active && !scan.processor.running);
    assert(session.result.valid && !cancels); /* Dispatcher owns immediate cancellation. */
}

static void test_stop_and_failures(void)
{
    setup();start("scope start 4");radar_scan_command(&scan,"scope stop");
    assert(scan.active && session.owns && !cancels);
    complete(UINT64_C(8000000000));poll();process();poll();
    assert(!scan.active && scan.count==1U && starts==1U && !qualifies && session.result.valid);
    setup();start("scope start 2");complete(UINT64_C(8000000000));poll();process();
    now+=3000000U;poll();
    assert(!scan.active && session.result.valid && !clears && !qualifies && !safes);
    setup();start("scope start 2");session.state=RADAR_SESSION_FAILED;
    poll();assert(!scan.active && session.owns && !cancels && !qualifies);
    setup();start("scope start 2");complete(UINT64_C(8000000000));poll();process();
    acknowledge();poll();radar_scan_command(&scan,"scope stop");
    assert(scan.active && sw3.job==SW3_OBSERVE); /* Finish native qualification. */
    sw3.job=SW3_IDLE;qualify_ready=1;poll();
    assert(!scan.active && starts==1U && !connects);
    setup();start("scope start 2");complete(UINT64_C(8000000000));poll();process();
    acknowledge();poll();sw3.job=SW3_IDLE;sw3.check_error=-1;poll();
    assert(!scan.active && starts==1U && !connects && !safes);
    setup();start("scope start 2");complete(UINT64_C(8000000000));poll();process();
    acknowledge();poll();sw3.job=SW3_IDLE;qualify_ready=1;poll();
    sw2.state=SW2_ONLINE;poll();assert(scan.state==RADAR_SCAN_QUERY);
    sw2.state=SW2_ONLINE;sw2.job=SW2_JOB_NONE;sw2.cli.state=RADAR_CLI_IDLE;
    sw2.cli_verified=1U;sw2.cli.version_epoch=2U;poll();
    assert(!scan.active && starts==1U); /* Stale version cannot start RF. */
    setup();start("scope start 2");complete(UINT64_C(8000000000));poll();process();
    acknowledge();poll();sw3.job=SW3_IDLE;qualify_ready=1;poll();
    radar_scan_command(&scan,"scope stop");assert(scan.active);
    sw2.state=SW2_ONLINE;poll();assert(!scan.active && starts==1U && safes==1U && !queries);
    setup();start("scope start 2");now+=30000000U;poll();
    assert(!scan.active && cancels==1U && session.owns && starts==1U);
}

static void test_bound_history_and_clock(void)
{
    u32 i;
    setup();scan.count=128U;scan.have_origin=1U;scan.origin_tick=100U;scan.last_tick=12700100U;
    scan.last_session=128U;scan.sys_hz=hz;session.next_session=129U;
    for(i=0U;i<128U;++i) {
        scan.history[i].session=i+1U;scan.history[i].timestamp_us=(u64)i*1000U;
    }
    start("scope start 32");
    for(i=0U;i<32U;++i) {
        complete(12800100U+(u64)i*200000U);poll();process();
        if(i+1U<32U) next_capture(); else acknowledge();
    }
    assert(!scan.active && starts==32U && scan.batch_done==32U && scan.count==128U);
    assert(qualifies==31U && connects==31U && queries==31U);
    assert(scan.history[0].session==33U && scan.history[127].session==160U);
    for(i=1U;i<128U;++i)
        assert(scan.history[i].timestamp_us>scan.history[i-1U].timestamp_us);
    radar_session_command(&session,"capture clear");qualify_ready=1;
    sw2.state=SW2_ONLINE;sw2.cli_verified=1U;hz=99999999U;
    radar_scan_command(&scan,"scope start 1");assert(!scan.active && starts==32U);
}

static void test_data_output(void)
{
    setup();make_retained();
    radar_scan_command(&scan,"scope data 0");assert(!data_headers);
    process();acknowledge();assert(!scan.active);
    radar_scan_command(&scan,"scope data 1");assert(!data_headers);
    radar_scan_command(&scan,"scope data 4294967296");assert(!data_headers);
    console_room=4095U;radar_scan_command(&scan,"scope data 0");assert(!data_headers);
    console_room=4096U;session.exporting=1U;
    radar_scan_command(&scan,"scope data 0");assert(!data_headers);
    session.exporting=0U;radar_scan_command(&scan,"scope data 0");
    assert(data_headers==1U && data_rows==16U && data_footers==1U);
    assert(scan.count==1U && starts==1U && session.result.valid);
}

static void test_live_roll_stop_resume(void)
{
    u32 i,revision,old_starts,old_clears;
    setup();start("scope live");
    assert(scan.live_mode && scan.batch_total==0U && !strcmp(scan.status,"LIVE"));
    for(i=0U;i<160U;++i) {
        revision=scan.revision;
        complete(UINT64_C(9000000000)+(u64)i*200000U);poll();
        assert(scan.revision==revision); /* No intermediate processing redraw. */
        process();assert(scan.revision!=revision);
        if(i+1U<160U) {
            revision=scan.revision;next_capture();
            assert(scan.revision==revision && !strcmp(scan.status,"LIVE"));
        }
    }
    assert(scan.active && starts==160U && scan.batch_done==160U && scan.count==128U);
    assert(scan.history[0].session==33U && scan.history[127].session==160U);
    for(i=1U;i<128U;++i)
        assert(scan.history[i].timestamp_us>scan.history[i-1U].timestamp_us);
    radar_scan_command(&scan,"scope stop");
    assert(!scan.active && !strcmp(scan.status,"LIVE STOPPED") && session.result.valid);
    old_starts=starts;old_clears=clears;
    radar_scan_command(&scan,"scope live");
    assert(scan.active && scan.live_mode && scan.state==RADAR_SCAN_WAIT_FRAME_PRESENT);
    poll();assert(starts==old_starts && clears==old_clears && session.result.valid);
    revision=scan.revision;next_capture();
    assert(scan.revision==revision && starts==161U && clears==160U);
    assert(sw2.check10_passes==1U && sw2.reset3_passes==1U);
    /* Saturated diagnostic counter must neither wrap nor end live mode. */
    scan.batch_done=UINT32_MAX;
    complete(UINT64_C(9032000000));poll();process();next_capture();
    assert(scan.active && scan.batch_done==UINT32_MAX && starts==162U);
    radar_scan_command(&scan,"scope stop");
    assert(scan.active && !strcmp(scan.status,"LIVE STOPPING") && session.owns && !cancels);
    complete(UINT64_C(9032200000));poll();process();poll();
    assert(!scan.active && scan.batch_done==UINT32_MAX && starts==162U);
    assert(!strcmp(scan.status,"LIVE STOPPED") && session.result.valid);
}

static void test_live_admission_failures(void)
{
    u32 old_starts;
    setup();session.confirmed=0U;radar_scan_command(&scan,"scope live");
    assert(!scan.active && !starts);
    setup();radar_session_start(&session);complete(UINT64_C(9500000000));
    radar_scan_command(&scan,"scope live");
    assert(!scan.active && session.result.valid && !clears); /* Unprocessed evidence. */
    radar_scan_command(&scan,"scope show");process();acknowledge();
    assert(scan.count==1U && !scan.active);
    sw2.cli.needs_reset=1U;radar_scan_command(&scan,"scope live");
    assert(!scan.active && !clears);
    sw2.cli.needs_reset=0U;control.desynced=1U;radar_scan_command(&scan,"scope live");
    assert(!scan.active && !clears);
    control.desynced=0U;session.result.crc32^=1U;radar_scan_command(&scan,"scope live");
    assert(!scan.active && !clears);
    session.result.crc32^=1U;radar_scan_command(&scan,"scope live");
    assert(scan.active);old_starts=starts;
    now+=3000000U;poll();
    assert(!scan.active && starts==old_starts && !clears && !qualifies);
    setup();start("scope live");session.state=RADAR_SESSION_FAILED;
    poll();old_starts=starts;poll();poll();
    assert(!scan.active && session.owns && starts==old_starts && !qualifies && !cancels);
    setup();start("scope live");complete(UINT64_C(9500000000));poll();process();
    acknowledge();poll();sw3.job=SW3_IDLE;sw3.check_error=-1;poll();poll();
    assert(!scan.active && starts==1U && qualifies==1U && !connects);
    setup();start("scope live");assert(radar_scan_command(&scan,"safe")==0);
    assert(!scan.active && !scan.live_mode && session.owns && !cancels);
    /* The dispatcher still owns immediate safe cancellation. */
    setup();session.next_session=UINT32_MAX;start("scope live");
    complete(UINT64_C(9500000000));poll();process();
    assert(session.next_session==0U);
    acknowledge();poll();sw3.job=SW3_IDLE;qualify_ready=1;poll();
    sw2.state=SW2_ONLINE;poll();sw2.state=SW2_ONLINE;sw2.job=SW2_JOB_NONE;
    sw2.cli.state=RADAR_CLI_IDLE;sw2.cli_verified=1U;poll();
    assert(!scan.active && starts==1U); /* Native wrap admission still gates RF. */
}

static void test_live_empty_resume(void)
{
    u32 stop_phase,old_clears;
    /* Paused before the first RF session: no history exists to identify yet. */
    setup();radar_scan_command(&scan,"scope live");
    assert(scan.state==RADAR_SCAN_WAIT_INITIAL_PRESENT && !starts);
    radar_scan_command(&scan,"scope stop");
    assert(!scan.active && !scan.count && !scan.last_session && !starts);
    assert(session.next_session==1U && !session.active_session && sw2.state==SW2_SAFE);
    radar_scan_command(&scan,"scope live");
    assert(scan.active && scan.resume_empty && scan.state==RADAR_SCAN_WAIT_FRAME_PRESENT);
    poll();assert(!starts && !qualifies && !clears);
    next_capture();assert(starts==1U && qualifies==1U && !clears);
    complete(UINT64_C(9600000000));poll();process();
    radar_scan_command(&scan,"scope stop");
    assert(!scan.active && scan.count==1U && session.result.valid);
    /* A failure record invalidates the pristine identity even if IDs are zero. */
    setup();radar_scan_command(&scan,"scope live");radar_scan_command(&scan,"scope stop");
    session.captures_failed=1U;radar_scan_command(&scan,"scope live");
    assert(!scan.active && !starts && !qualifies);
    for(stop_phase=0U;stop_phase<3U;++stop_phase) {
        setup();start("scope live");complete(UINT64_C(9700000000));poll();process();
        acknowledge();poll();
        assert(scan.state==RADAR_SCAN_QUALIFY && !session.result.valid && clears==1U);
        if(stop_phase>0U) {
            sw3.job=SW3_IDLE;qualify_ready=1;poll();
            assert(scan.state==RADAR_SCAN_CONNECT);
        }
        if(stop_phase>1U) {
            sw2.state=SW2_ONLINE;poll();assert(scan.state==RADAR_SCAN_QUERY);
        }
        radar_scan_command(&scan,"scope stop");
        assert(scan.active && starts==1U); /* Never capture again just to stop. */
        if(stop_phase==0U) { sw3.job=SW3_IDLE;qualify_ready=1; }
        else if(stop_phase==1U) sw2.state=SW2_ONLINE;
        else {
            sw2.state=SW2_ONLINE;sw2.job=SW2_JOB_NONE;
            sw2.cli.state=RADAR_CLI_IDLE;sw2.cli_verified=1U;
        }
        poll();
        assert(!scan.active && !strcmp(scan.status,"LIVE STOPPED") && starts==1U);
        assert(!session.result.valid && session.state==RADAR_SESSION_IDLE && sw2.state==SW2_SAFE);
        old_clears=clears;
        radar_scan_command(&scan,"scope live");
        assert(scan.active && scan.resume_empty && scan.state==RADAR_SCAN_WAIT_FRAME_PRESENT);
        poll();assert(starts==1U && qualifies==1U && clears==old_clears);
        next_capture();
        assert(starts==2U && qualifies==2U && clears==old_clears && !scan.resume_empty);
        assert(sw2.check10_passes==1U && sw2.reset3_passes==1U);
        radar_scan_command(&scan,"scope stop");
        complete(UINT64_C(9702000000));poll();process();poll();
        assert(!scan.active && scan.count==2U && starts==2U && session.result.valid);
    }
    /* Identical empty native state cannot make FAILED an implicit warm resume. */
    setup();start("scope live");complete(UINT64_C(9700000000));poll();process();
    acknowledge();poll();sw3.job=SW3_IDLE;qualify_ready=1;
    radar_scan_command(&scan,"scope stop");
    assert(!scan.active && !session.result.valid && sw2.state==SW2_SAFE);
    scan.status="FAILED";radar_scan_command(&scan,"scope live");
    assert(!scan.active && starts==1U && qualifies==1U);
    scan.status="LIVE STOPPED";control.desynced=1U;
    radar_scan_command(&scan,"scope live");assert(!scan.active && qualifies==1U);
    control.desynced=0U;session.active_session++;
    radar_scan_command(&scan,"scope live");assert(!scan.active && qualifies==1U);
}

static void test_spatial_publication_and_failure(void)
{
    unsigned i;
    u32 rev;
    setup();make_retained();rev=scan.revision;
    for(i=0U;i<RADAR_RANGE_CHIRPS;++i) poll();
    assert(scan.state==RADAR_SCAN_PROCESS && !scan.count && scan.revision==rev);
    assert(!scan.published_session && spatial_polls==1U);
    session.result.crc32^=1U;poll();
    assert(!scan.active && !scan.count && !scan.published_session);
    setup();make_retained();spatial_bad=1;
    while(scan.active && scan.state==RADAR_SCAN_PROCESS) poll();
    assert(scan.state==RADAR_SCAN_WAIT_FRAME_PRESENT && scan.count==1U);
    assert(scan.history[0].valid && !scan.spatial_frame.valid);
    assert(scan.spatial_frame.session==scan.history[0].session && scan.spatial_frame.error<0);
    assert(starts==1U && session.result.valid);
    acknowledge();radar_scan_command(&scan,"scope clear");
    assert(!scan.count && !scan.published_session && !scan.spatial_frame.session);
    for(i=0U;i<2U;++i) {
        setup();
        if(i) spatial_begin_bad=1; else spatial_sink_bad=1;
        make_retained();
        while(scan.active && scan.state==RADAR_SCAN_PROCESS) poll();
        assert(scan.state==RADAR_SCAN_WAIT_FRAME_PRESENT && scan.count==1U);
        assert(scan.history[0].valid && !scan.spatial_frame.valid && scan.spatial_frame.error<0);
        assert(scan.spatial_frame.session==scan.history[0].session);
    }
    setup();start("scope start 1");complete(UINT64_C(7000000000));
    now+=5000000U;poll();process();
    assert(scan.published_observed_us==session.result.complete_us);
    assert(now-scan.published_observed_us>=5000000U);
}

static void spatial_range_prefix(void)
{
    unsigned i;
    make_retained();
    for(i=0U;i<RADAR_RANGE_CHIRPS-1U;++i) poll();
    assert(scan.state==RADAR_SCAN_PROCESS && scan.processing_record==RADAR_RANGE_CHIRPS-1U);
    assert(!spatial_polls && !scan.spatial_calls && !scan.spatial_slices && !scan.count);
}

static void test_spatial_slice_budget(void)
{
    unsigned validated,mutation;
    u32 revision;
    /* Guard time consumes the same work budget:100us guard +4x100us
     * atomic steps, rather than five steps after the guard. */
    setup();spatial_range_prefix();revision=scan.revision;
    spatial_done_after=10U;spatial_step_us=100U;profile_validation_us=100U;
    validated=profile_validations;poll();
    assert(profile_validations==validated+1U);
    assert(scan.spatial_calls==4U && scan.spatial_slices==1U && scan.spatial_max_slice_us==500U);
    assert(scan.state==RADAR_SCAN_PROCESS && !scan.count && scan.revision==revision);
    poll();assert(scan.spatial_calls==8U && scan.spatial_slices==2U && !scan.count);
    poll();assert(scan.spatial_calls==10U && scan.spatial_slices==3U && scan.spatial_max_slice_us==500U);
    assert(scan.state==RADAR_SCAN_WAIT_FRAME_PRESENT && scan.count==1U && scan.spatial_frame.valid);
    assert(scan.revision!=revision && starts==1U && !clears && !qualifies);
    poll();assert(starts==1U && !clears && scan.active); /* Still needs actual display ACK. */
    acknowledge();assert(!scan.active && session.result.valid);

    /* Completion on call257 cannot leak a partial publication at call256.
     * A stationary timer is still bounded by the independent call limit. */
    setup();spatial_range_prefix();spatial_step_us=0U;spatial_done_after=257U;
    validated=profile_validations;poll();
    assert(profile_validations==validated+1U && scan.spatial_calls==256U && scan.spatial_slices==1U);
    assert(scan.state==RADAR_SCAN_PROCESS && !scan.count && !scan.published_session);
    poll();assert(scan.spatial_calls==257U && scan.spatial_slices==2U && scan.count==1U);
    assert(scan.state==RADAR_SCAN_WAIT_FRAME_PRESENT && scan.spatial_max_slice_us==0U);

    /* An atomic step may end beyond the deadline, but no next step starts.
     * The observed overrun is reported rather than hidden or clamped. */
    setup();spatial_range_prefix();spatial_step_us=100U;spatial_done_after=20U;
    profile_validation_us=150U;poll();
    assert(scan.spatial_calls==4U && scan.spatial_max_slice_us==550U && !scan.count);

    /* If the guard or the final range chirp already used the budget, defer
     * space work while keeping the completed range and CRC unchanged. */
    setup();spatial_range_prefix();profile_validation_us=500U;spatial_step_us=0U;
    poll();assert(!scan.spatial_calls && scan.spatial_slices==1U && scan.spatial_max_slice_us==500U);
    assert(scan.processing_record==RADAR_RANGE_CHIRPS && !scan.count);
    profile_validation_us=0U;poll();assert(scan.spatial_calls==3U && scan.count==1U);
    setup();spatial_range_prefix();range_step_us=600U;spatial_step_us=0U;
    poll();assert(!scan.spatial_calls && scan.spatial_max_slice_us==600U && !scan.count);
    range_step_us=0U;poll();assert(scan.spatial_calls==3U && scan.count==1U);

    /* Numeric failure mid-batch preserves this frame's valid range result,
     * publishes invalid spatial data once, and never invokes another step. */
    setup();spatial_range_prefix();spatial_step_us=50U;spatial_done_after=20U;spatial_fail_at=3U;
    poll();assert(scan.spatial_calls==3U && scan.spatial_slices==1U && scan.spatial_max_slice_us==150U);
    assert(scan.count==1U && scan.history[0].valid && !scan.spatial_frame.valid && scan.spatial_frame.error<0);
    assert(scan.state==RADAR_SCAN_WAIT_FRAME_PRESENT && session.result.valid);

    /* All original ownership/identity guards remain at every slice boundary. */
    for(mutation=0U;mutation<4U;++mutation) {
        setup();spatial_range_prefix();spatial_step_us=0U;spatial_done_after=600U;
        poll();assert(scan.spatial_calls==256U && !scan.count);
        if(mutation==0U) ++session.result.session;
        else if(mutation==1U) session.result.crc32^=1U;
        else if(mutation==2U) session.owns=1U;
        else profile.cfg_sha256="changed_between_slices";
        poll();assert(!scan.active && !scan.count && !scan.published_session);
        assert(scan.spatial_calls==256U && spatial_polls==256U && starts==1U && !clears);
    }
}

static void test_transport_batches(void)
{
    unsigned frames,frame,n,missing;
    for(frames=8U;frames<=16U;frames*=2U)for(missing=0U;missing<3U;++missing) {
        setup();profile.packet_count=frames;capture_store=batch_raw;
        memset(batch_raw,0x5a,sizeof batch_raw);
        assert(radar_scan_command(&scan,"scope display decoupled")==1);
        assert(radar_scan_command(&scan,frames==8U?"scope start 8":"scope start 16")==1);
        poll();assert(starts==1U);complete(UINT64_C(7000000000));
        session.result.packet_count=frames;session.result.bytes=frames*RADAR_RANGE_INPUT_BYTES;
        session.result.crc32=radar_crc32_update(UINT32_MAX,batch_raw,session.result.bytes)^UINT32_MAX;
        for(frame=0U;frame<frames;++frame) {
            RadarTransportTime *t=&session.result.frame_transport[frame];
            session.result.frame_crc32[frame]=radar_crc32_update(UINT32_MAX,batch_raw+frame*RADAR_RANGE_INPUT_BYTES,RADAR_RANGE_INPUT_BYTES)^UINT32_MAX;
            session.result.frame_observed_us[frame]=now;
            t->sequence=frame+1U;t->bytes=RADAR_RANGE_INPUT_BYTES;t->hz=100000000U;
            t->first_tick=UINT64_C(9000000000)+frame*UINT64_C(10000000);t->last_tick=t->first_tick+100U;
            t->valid=(uint8_t)(missing==0U || (missing==2U && frame!=2U));
        }
        for(n=0U;n<frames*110U+10U && scan.active;++n)poll();
        assert(!scan.active&&scan.count==frames&&scan.batch_done==frames);
        assert(starts==1U&&!clears&&!qualifies&&session.result.valid);
        assert(!scan.frames.count&&!scan.frames.lease&&scan.frames.high_water==frames);
        assert(scan.presented_revision==0U);
        for(frame=0U;frame<frames;++frame) {
            const RadarTimingRecord *timing=radar_timing_get(&scan.timing,frame);
            assert(scan.history[frame].session==frame+1U);
            int unavailable=missing==1U || (missing==2U&&frame==2U);
            assert(scan.history[frame].timestamp_us==(unavailable?0U:frame*UINT64_C(100000)));
            assert(!!scan.history[frame].time_untrusted==unavailable);
            assert(timing&&timing->frame_id==frame+1U&&timing->frame_index==frame);
            assert(timing->transport_sequence==frame+1U&&timing->transport_hz==100000000U);
            assert(timing->transport_first_tick==UINT64_C(9000000000)+frame*UINT64_C(10000000));
            assert(!!(timing->stages&RADAR_TIMING_TRANSPORT)==!unavailable);
            assert(timing->observed_ps_us==session.result.frame_observed_us[frame]);
            assert(timing->stages&RADAR_TIMING_PUBLISHED);
        }
        assert(!!(scan.processing_frame.flags&RADAR_FRAME_TIME_TRUSTED)==(missing!=1U));
        assert(!!(scan.processing_frame.flags&RADAR_FRAME_TIME_TRANSPORT)==(missing!=1U));
    }
    setup();assert(radar_scan_command(&scan,"scope backend PL_BASELINE")==1);
    assert(scan.backend==PS_REFERENCE&&!starts);
}

static void complete_batch(unsigned frames)
{
    unsigned i;u64 tick=UINT64_C(7000000000)+session.active_session*frames*UINT64_C(10000000);
    assert(!session.raw_store_owner);memset(batch_raw,0x50+(int)session.active_session,sizeof batch_raw);
    complete(tick);session.result.packet_count=frames;session.result.bytes=frames*RADAR_RANGE_INPUT_BYTES;
    session.result.crc32=radar_crc32_update(UINT32_MAX,batch_raw,session.result.bytes)^UINT32_MAX;
    for(i=0U;i<frames;++i) {
        RadarTransportTime *t=&session.result.frame_transport[i];
        session.result.frame_crc32[i]=radar_crc32_update(UINT32_MAX,batch_raw+i*RADAR_RANGE_INPUT_BYTES,RADAR_RANGE_INPUT_BYTES)^UINT32_MAX;
        session.result.frame_observed_us[i]=now;
        t->sequence=i+1U;t->bytes=RADAR_RANGE_INPUT_BYTES;t->hz=100000000U;
        t->first_tick=tick+i*UINT64_C(10000000);t->last_tick=t->first_tick+100U;t->valid=1U;
    }
}
static void test_producer_consumer_overlap(void)
{
    unsigned frames,n,overlap,waiting,stopped;
    for(frames=8U;frames<=16U;frames*=2U) {
        setup();profile.packet_count=frames;capture_store=batch_raw;
        radar_scan_command(&scan,"scope display decoupled");radar_scan_command(&scan,"scope pipeline on");
        assert(scan.pipeline);radar_scan_command(&scan,"scope live");
        overlap=waiting=stopped=0U;
        for(n=0U;n<20000U&&scan.active;++n) {
            if(sw3.job==SW3_OBSERVE){sw3.job=SW3_IDLE;qualify_ready=1;}
            if(sw2.state==SW2_CONNECT_WAIT)sw2.state=SW2_ONLINE;
            if(sw2.job==SW2_JOB_QUERY){sw2.job=SW2_JOB_NONE;sw2.state=SW2_ONLINE;sw2.cli.state=RADAR_CLI_IDLE;sw2.cli_verified=1;}
            if(session.owns) {
                if(session.raw_store_owner) {
                    session.state=RADAR_SESSION_COPY;++waiting;++session.copy_wait_polls;
                    assert(batch_raw[0]==0x50+session.raw_store_owner);
                } else complete_batch(frames);
            }
            if(session.owns&&scan.state==RADAR_SCAN_PROCESS)++overlap;
            poll();
            if(scan.count>=64U&&!stopped){radar_scan_command(&scan,"scope stop");stopped=1U;}
        }
        assert(stopped&&!scan.active&&scan.count==64U);
        assert(overlap>64U&&waiting>64U&&scan.prefetched_batches>=64U/frames-1U);
        assert(scan.frames.high_water==frames&&!scan.frames.count&&!scan.frames.lease&&!session.raw_store_owner);
        assert(session.result.valid&&!session.owns&&session.source==RADAR_SOURCE_STOPPED);
        for(n=0U;n<64U;++n)assert(scan.history[n].session==n+1U);
        assert(!scan.presented_revision); /* Missing display ACK never grants capture memory. */
    }
}

static void test_production_pl_branch(void)
{
    unsigned mode,n;
    void *mapped=mmap((void *)(uintptr_t)RADAR_DSP_WORK_BASE,RADAR_DSP_WORK_END-RADAR_DSP_WORK_BASE,
        PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
    assert(mapped==(void *)(uintptr_t)RADAR_DSP_WORK_BASE);
    for(mode=0U;mode<5U;++mode) {
        setup();pl_enabled=1U;radar_dsp_platform_init(&scan.dsp);
        radar_scan_command(&scan,mode==0U?"scope backend PL_BASELINE":"scope backend PL_ENHANCED");
        assert(scan.backend==(mode==0U?PL_BASELINE:PL_ENHANCED));
        if(mode==2U)pl_flags=4U;
        if(mode==3U)pl_bad_header=1U;
        if(mode==4U)pl_flags=0x4c0U; /* Missing trustworthy time falls back, never silently enhanced. */
        radar_scan_command(&scan,"scope display decoupled");radar_scan_command(&scan,"scope start 1");
        poll();complete(UINT64_C(7000000000));
        for(n=0U;n<400U&&scan.active;++n)poll();
        assert(!scan.active&&!ps_range_calls&&pl_pack_calls==96U&&pl_power_imports==1U);
        if(mode==3U)assert(!scan.count&&!strcmp(scan.status,"FAILED"));
        else {
            RadarSampleFreshness published;u32 revision;
            RadarTimingRecord *timing=radar_timing_current(&scan.timing);
            RadarTimingAck ack={0};
            assert(scan.count==1U&&scan.history[0].fft_count==384U);
            assert(timing&&timing->frame_id==scan.history[0].session);
            assert((timing->stages&0x3fU)==0x3fU&&timing->pl_cycles==UINT64_C(4304843839));
            assert(timing->front_begin_ps_us>=timing->processing_begin_ps_us);
            assert(timing->front_end_ps_us>=timing->front_begin_ps_us);
            assert(timing->submit_begin_ps_us>=timing->front_end_ps_us);
            assert(timing->done_ps_us>=timing->submit_end_ps_us);
            assert(timing->post_done_ps_us>=timing->done_ps_us);
            assert(timing->published_ps_us>=timing->post_done_ps_us);
            assert(pl_rows==(mode==2U?0U:256U));
            assert(!!scan.spatial_frame.valid==(mode!=2U));
            assert(scan.published_freshness.frame_id==scan.published_session);
            assert(scan.published_freshness.backend==scan.backend&&scan.published_freshness.result_flags==pl_flags);
            assert(scan.published_freshness.spatial_valid==scan.spatial_frame.valid);
            assert(!!scan.published_freshness.deadline_valid==(mode!=2U&&mode!=4U));
            published=scan.published_freshness;revision=scan.revision;
            now+=600000U;radar_scan_command(&scan,"scope status");
            assert(!memcmp(&published,&scan.published_freshness,sizeof published)&&scan.revision==revision);
            ack.sequence=1U;ack.frame_id=timing->frame_id;ack.data_revision=scan.revision;
            ack.render_begin_ps_us=now;ack.render_end_ps_us=now+10U;
            ack.queue_ps_us=now+20U;ack.ack_ps_us=now+30U;
            radar_scan_display_ack(&scan,&ack);radar_scan_display_ack(&scan,&ack);
            assert(scan.timing.first_presented==1U&&scan.presented_revision==0U);
            timing_headers=timing_footers=timing_blocks=0U;
            assert(radar_scan_command(&scan,"scope timing 0")==1);
            assert(timing_headers==1U&&timing_footers==1U&&!timing_blocks);
            scan.active=1U;radar_scan_command(&scan,"scope timing 0");
            radar_scan_command(&scan,"scope timing clear");scan.active=0U;
            assert(timing_blocks==2U&&scan.timing.count==1U);
            radar_scan_command(&scan,"scope timing clear");
            assert(!scan.timing.count&&scan.revision==revision&&session.result.valid);
        }
    }
    assert(munmap(mapped,RADAR_DSP_WORK_END-RADAR_DSP_WORK_BASE)==0);
}
int main(void)
{
    assert(radar_scan_alpha_q31(0U)==0U);
    assert(radar_scan_alpha_q31(1U)==1074U);
    assert(radar_scan_alpha_q31(100000U)==104734013U);
    assert(radar_scan_alpha_q31(500000U)==475021701U);
    assert(radar_scan_alpha_q31(2000000U)==1357468564U);
    assert(radar_scan_alpha_q31(10000000U)==2133014017U);
    assert(radar_scan_alpha_q31(UINT64_MAX)==UINT32_C(0x80000000));
    test_admission_and_display();
    test_two_frames_and_retention();
    test_corruption_and_commands();
    test_stop_and_failures();
    test_bound_history_and_clock();
    test_data_output();
    test_live_roll_stop_resume();
    test_live_admission_failures();
    test_live_empty_resume();
    test_spatial_publication_and_failure();
    test_spatial_slice_budget();
    test_transport_batches();
    test_producer_consumer_overlap();
    test_production_pl_branch();
    puts("PASS: scheduler admission, display ack, finite/live batches, 160-frame rollover, retained/empty resume, ownership, CRC, timestamps, stop/failure, retention, history, data output, guarded500us/256-step spatial slices");
    return 0;
}
