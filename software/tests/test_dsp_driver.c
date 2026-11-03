/* SPDX-License-Identifier: MIT: production CSR driver with injected hardware. */
#define _GNU_SOURCE
#include "radar_dsp.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
typedef struct { uint32_t reg[80],reads,writes,flushes,invalidates,barriers;uint64_t time; } Model;
static uint32_t read_reg(void *c,uint32_t a){Model *m=c;assert(!(a&3U)&&a<320U);++m->reads;return m->reg[a/4U];}
static void write_reg(void *c,uint32_t a,uint32_t v){Model *m=c;++m->writes;
    if(a==8U){if(v==4U)m->reg[3]=0U;else if(v==1U || v==8U)m->reg[3]=1U;}
    else if(a==0x11cU){if(v==2U)m->reg[64]=0U;else if(v==1U){assert(m->reg[64]&127U);--m->reg[64];}}
    else m->reg[a/4U]=v;
}
static void flush(void *c,uint32_t a,uint32_t n){Model *m=c;assert(!(a&63U)&&!(n&63U));++m->flushes;}
static void inv(void *c,uint32_t a,uint32_t n){Model *m=c;assert(!(a&63U)&&!(n&63U));assert(!(m->reg[3]&1U));++m->invalidates;}
static void barrier(void *c){++((Model *)c)->barriers;}
static uint64_t now(void *c){return ((Model *)c)->time;}
static RadarDspJob job(void){RadarDspJob j={0};j.backend=PL_BASELINE;j.job_id=j.config_epoch=1U;
    memset(j.frame.cfg_sha256,'a',64U);memset(j.frame.profile_id,'b',16U);
    j.frame.frame_id=j.frame.acquisition_id=1U;j.frame.bytes=399360U;j.frame.flags=7U;radar_dsp_job_layout(&j);return j;}
static void initialize(Model *m,RadarDspDriver *d){RadarDspIo io={m,read_reg,write_reg,flush,inv,barrier,now,1};
    memset(m,0,sizeof *m);m->reg[0]=RADAR_DSP_MAGIC;m->reg[1]=RADAR_DSP_ABI;radar_dsp_init(d,&io);}
static void seed_header(uint32_t epoch,uint64_t time,uint32_t acquisition,uint32_t index)
{
    uint32_t *h=(uint32_t *)(uintptr_t)RADAR_DSP_TEMPORAL;
    memset(h,0,64U);h[0]=0x544d5031U;h[1]=epoch;h[2]=acquisition;h[3]=index;
    h[4]=(uint32_t)time;h[5]=(uint32_t)(time>>32);h[14]=0x00010001U;
    h[16]=0xabcdef01U; /* First cell is outside the explicit header write. */
}
static int header_identity_rejected(const RadarDspJob *j)
{
    /* Only RTL's ordering predicate is modeled here, not cell mathematics. */
    const uint32_t *h=(const uint32_t *)(uintptr_t)RADAR_DSP_TEMPORAL;
    const uint64_t last=(uint64_t)h[4]|((uint64_t)h[5]<<32);
    return h[0]==0x544d5031U && ((j->frame.flags&4U && j->frame.timestamp_us<=last) ||
        j->frame.acquisition_id<h[2] || (j->frame.acquisition_id==h[2] && j->frame.frame_index<=h[3]));
}
static void finish_success(Model *m,RadarDspDriver *d)
{ m->reg[3]=2U;m->reg[0x78/4]=3456U;assert(radar_dsp_poll(d)==RADAR_DSP_DONE); }
static void test_temporal_epoch_header(void)
{
    Model m;RadarDspDriver d;RadarDspJob j=job(),bad;uint32_t before[16],writes;
    uint32_t *h=(uint32_t *)(uintptr_t)RADAR_DSP_TEMPORAL;
    initialize(&m,&d);j.backend=PL_ENHANCED;j.frame.timestamp_us=100000U;
    seed_header(77U,900000U,99U,9U); /* NOLOAD/canceled state, software guard still empty. */
    assert(radar_dsp_submit(&d,&j)==0);assert(!header_identity_rejected(&j));
    assert(h[0]==0U&&h[16]==0xabcdef01U);finish_success(&m,&d);
    seed_header(j.config_epoch,j.frame.timestamp_us,j.frame.acquisition_id,j.frame.frame_index);
    bad=j;++bad.job_id;++bad.config_epoch;bad.temporal_control=1U;
    memcpy(before,h,64U);writes=m.writes;
    assert(radar_dsp_submit(&d,&bad)==RADAR_DSP_E_IDENTITY);
    assert(m.writes==writes&&!memcmp(before,h,64U)); /* Reset cannot revive duplicate. */
    ++j.job_id;++j.config_epoch;++j.frame.acquisition_id;j.frame.timestamp_us=0U;
    assert(header_identity_rejected(&j)); /* Reproduce scope-clear old-header ordering. */
    assert(radar_dsp_submit(&d,&j)==0);assert(!header_identity_rejected(&j)&&h[0]==0U);
    finish_success(&m,&d);
    seed_header(j.config_epoch,100000U,j.frame.acquisition_id,j.frame.frame_index);
    ++j.job_id;++j.frame.frame_index;j.frame.timestamp_us=1U;
    assert(radar_dsp_submit(&d,&j)==0);assert(header_identity_rejected(&j));
    /* Same epoch regression still reaches RTL failure; it is not erased. */
    m.reg[3]=6U;m.reg[4]=0x301U;assert(radar_dsp_poll(&d)==RADAR_DSP_E_HARDWARE);
    initialize(&m,&d);j=job();j.backend=PL_ENHANCED;
    assert(radar_dsp_submit(&d,&j)==0);finish_success(&m,&d);
    seed_header(j.config_epoch,900000U,j.frame.acquisition_id,j.frame.frame_index);
    ++j.job_id;++j.config_epoch;++j.frame.acquisition_id;j.backend=PL_BASELINE;
    assert(radar_dsp_submit(&d,&j)==0);assert(h[0]==0x544d5031U);finish_success(&m,&d);
    assert(d.need_temporal_reset);
    ++j.job_id;++j.frame.frame_index;j.backend=PL_ENHANCED;j.frame.timestamp_us=0U;
    assert(radar_dsp_submit(&d,&j)==0);assert(h[0]==0U&&!header_identity_rejected(&j));finish_success(&m,&d);
    seed_header(j.config_epoch,900000U,j.frame.acquisition_id,j.frame.frame_index);
    ++j.job_id;++j.frame.frame_index;j.temporal_control=1U;
    assert(radar_dsp_submit(&d,&j)==0);assert(h[0]==0U&&!header_identity_rejected(&j));finish_success(&m,&d);
    puts("PASS temporal header: stale initial state, scope epoch/time reset, baseline bridge, manual reset, duplicate and same-epoch regression");
}
static void test_two_slot_leases(void)
{
    Model m;RadarDspDriver d;RadarDspJob a=job(),b;unsigned writes;
    initialize(&m,&d);
    assert(!radar_dsp_job_layout_slot(&a,0U,1U));
    a.frame.capture_epoch=a.frame.acquisition_id;a.frame.transport_sequence=1U;
    a.frame.raw_lease=17U;a.frame.raw_slot=3U;
    assert(!radar_dsp_submit(&d,&a));
    b=a;++b.job_id;++b.frame.frame_index;++b.frame.frame_id;++b.frame.transport_sequence;
    b.frame.raw_slot=4U;b.frame.raw_lease=18U;assert(!radar_dsp_job_layout_slot(&b,1U,2U));
    assert(b.input==0x02900000U && b.temporal_state+RADAR_DSP_TEMPORAL_BYTES<=RADAR_DSP_VIEW_SLOT0);
    writes=m.writes;assert(radar_dsp_submit(&d,&b)==RADAR_DSP_E_OWNER);assert(m.writes==writes);
    finish_success(&m,&d);assert(!radar_dsp_submit(&d,&b));finish_success(&m,&d);
    ++b.job_id;++b.frame.frame_index;++b.frame.frame_id;++b.frame.transport_sequence;
    writes=m.writes;assert(radar_dsp_submit(&d,&b)==RADAR_DSP_E_OWNER);assert(m.writes==writes);
    assert(!radar_dsp_job_layout_slot(&b,0U,3U));b.frame.raw_lease=19U;
    b.frame.transport_sequence++;assert(radar_dsp_submit(&d,&b)==RADAR_DSP_E_IDENTITY);
    --b.frame.transport_sequence;assert(!radar_dsp_submit(&d,&b));finish_success(&m,&d);
    assert(radar_dsp_job_layout_slot(&b,2U,4U)==RADAR_DSP_E_ARGUMENT);
    puts("PASS two DSP slots: exact non-overlap, engine exclusion, stale lease and source identity refusal");
}
int main(void)
{
    Model m;RadarDspDriver d;RadarDspJob j=job(),bad;RadarTransportTime t;
    void *mapped=mmap((void *)(uintptr_t)RADAR_DSP_TEMPORAL,4096U,PROT_READ|PROT_WRITE,
        MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
    assert(mapped==(void *)(uintptr_t)RADAR_DSP_TEMPORAL);
    test_temporal_epoch_header();
    test_two_slot_leases();
    initialize(&m,&d);d.io.mapped=0;assert(radar_dsp_probe(&d)==RADAR_DSP_E_UNAVAILABLE);assert(m.reads==0U);
    d.io.mapped=1;m.reg[1]++;assert(radar_dsp_probe(&d)==RADAR_DSP_E_IDENTITY);m.reg[1]=RADAR_DSP_ABI;
    bad=j;bad.cube_out=bad.input;assert(radar_dsp_submit(&d,&bad)==RADAR_DSP_E_MEMORY);assert(m.writes==0U);
    bad=j;bad.input++;assert(radar_dsp_submit(&d,&bad)==RADAR_DSP_E_MEMORY);
    bad=j;bad.frame.flags=1U;assert(radar_dsp_submit(&d,&bad)==RADAR_DSP_E_ARGUMENT);
    bad=j;bad.frame.cfg_sha256[1]='x';assert(radar_dsp_submit(&d,&bad)==RADAR_DSP_E_ARGUMENT);
    bad=j;bad.numeric_format=2U;assert(radar_dsp_submit(&d,&bad)==RADAR_DSP_E_ARGUMENT);
    bad=j;bad.cube_capacity-=64U;assert(radar_dsp_submit(&d,&bad)==RADAR_DSP_E_MEMORY);
    assert(radar_dsp_submit(&d,&j)==0);assert(m.flushes==7U&&m.invalidates==0U);
    assert(m.reg[8]==j.input&&m.reg[9]==j.range_tmp&&m.reg[15]==786432U);
    assert(m.reg[0x64/4]==98304U&&m.reg[0x60/4]==1038676U&&m.reg[0x5c/4]==1038676U);
    assert(radar_dsp_submit(&d,&j)==RADAR_DSP_E_OWNER);assert(radar_dsp_poll(&d)==0);
    m.reg[3]=2U;m.reg[0x78/4]=3456U;m.reg[0x68/4]=1234U;
    assert(radar_dsp_poll(&d)==RADAR_DSP_DONE);assert(m.invalidates==5U);
    assert(radar_dsp_result(&d)->cycles==1234U&&radar_dsp_result(&d)->valid);
    assert(!strcmp(radar_dsp_result(&d)->cfg_sha256,j.frame.cfg_sha256)&&d.result.numeric_format==RADAR_DSP_NUMERIC_Q2_22);
    bad=j;bad.frame.cfg_sha256[0]='c';++bad.frame.frame_index;
    assert(radar_dsp_submit(&d,&bad)==RADAR_DSP_E_IDENTITY);
    j.job_id=2U;assert(radar_dsp_submit(&d,&j)==RADAR_DSP_E_IDENTITY);
    j.frame.acquisition_id=2U;assert(radar_dsp_submit(&d,&j)==0);assert(radar_dsp_cancel(&d)==0);
    assert(radar_dsp_poll(&d)==0);assert(m.invalidates==5U);
    m.reg[3]=6U;assert(radar_dsp_poll(&d)==RADAR_DSP_E_CANCELED);assert(!radar_dsp_result(&d));
    initialize(&m,&d);j=job();assert(radar_dsp_submit(&d,&j)==0);m.time=5000000U;
    assert(radar_dsp_poll(&d)==0&&d.state==RADAR_DSP_CANCELING);m.time+=100000U;
    assert(radar_dsp_poll(&d)==RADAR_DSP_E_LOCKED);assert(radar_dsp_submit(&d,&j)==RADAR_DSP_E_LOCKED);
    assert(radar_dsp_recover(&d)==RADAR_DSP_E_OWNER); /* Busy timeout never forces reset. */
    initialize(&m,&d);assert(radar_dsp_submit(&d,&j)==0);m.reg[3]=6U;m.reg[4]=0x80000001U;
    assert(radar_dsp_poll(&d)==RADAR_DSP_E_LOCKED&&d.recoverable);
    assert(radar_dsp_recover(&d)==0&&d.state==RADAR_DSP_RECOVERING);
    assert(radar_dsp_poll(&d)==0);m.reg[3]=m.reg[4]=0U;
    assert(radar_dsp_poll(&d)==RADAR_DSP_DONE&&d.state==RADAR_DSP_IDLE);
    assert(!radar_dsp_result(&d));assert(radar_dsp_submit(&d,&j)==RADAR_DSP_E_IDENTITY);
    ++j.frame.acquisition_id;assert(radar_dsp_submit(&d,&j)==0);
    initialize(&m,&d);assert(radar_dsp_submit(&d,&j)==0);m.reg[3]=2U;m.reg[0x78/4]=383U;
    assert(radar_dsp_poll(&d)==RADAR_DSP_E_RESULT&&m.invalidates==0U);
    /* Partial temporal header commit can leave plausible magic/epoch in DDR.
     * Cache invalidation cannot repair semantic state: explicit reset remains
     * sticky through recovery and an intervening baseline at the same epoch. */
    initialize(&m,&d);j=job();j.backend=PL_ENHANCED;
    assert(radar_dsp_submit(&d,&j)==0&&m.reg[0x58/4]==1U);
    m.reg[3]=2U;m.reg[0x78/4]=3456U;assert(radar_dsp_poll(&d)==1&&!d.need_temporal_reset);
    ++j.frame.frame_index;++j.job_id;assert(radar_dsp_submit(&d,&j)==0&&m.reg[0x58/4]==0U);
    m.reg[3]=6U;m.reg[4]=0x80000001U;assert(radar_dsp_poll(&d)==RADAR_DSP_E_LOCKED&&d.need_temporal_reset);
    assert(radar_dsp_recover(&d)==0);m.reg[3]=m.reg[4]=0U;assert(radar_dsp_poll(&d)==1&&d.need_temporal_reset);
    ++j.frame.frame_index;++j.job_id;j.backend=PL_BASELINE;assert(radar_dsp_submit(&d,&j)==0);
    m.reg[3]=2U;m.reg[0x78/4]=3456U;assert(radar_dsp_poll(&d)==1&&d.need_temporal_reset);
    ++j.frame.frame_index;++j.job_id;j.backend=PL_ENHANCED;assert(radar_dsp_submit(&d,&j)==0&&m.reg[0x58/4]==1U);
    m.reg[3]=2U;assert(radar_dsp_poll(&d)==1&&!d.need_temporal_reset);
    initialize(&m,&d);assert(radar_dsp_submit(&d,&j)==0);m.reg[3]=2U;m.reg[0x78/4]=3456U;m.reg[0x7c/4]=512U;
    assert(radar_dsp_poll(&d)==RADAR_DSP_E_RESULT&&m.invalidates==0U);
    initialize(&m,&d);m.reg[3]=1U;
    assert(radar_dsp_metadata_clear(&d)==0);assert(radar_dsp_metadata_pop(&d,&t)==0);
    m.reg[64]=1;m.reg[0x108/4]=7U;m.reg[0x10c/4]=100U;m.reg[0x114/4]=200U;
    m.reg[0x120/4]=399360U;m.reg[0x124/4]=99999001U;
    assert(radar_dsp_metadata_pop(&d,&t)==1&&t.valid&&t.sequence==7U&&t.first_tick==100U&&t.bytes==399360U);
    m.reg[64]=0x80000001U;assert(radar_dsp_metadata_pop(&d,&t)==1&&!t.valid);
    assert(munmap(mapped,4096U)==0);
    puts("PASS production DSP driver ownership, bounds, cancel/drain, cache and metadata");return 0;
}
