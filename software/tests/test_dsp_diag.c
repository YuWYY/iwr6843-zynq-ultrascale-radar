/* SPDX-License-Identifier: MIT
 * Production diagnostic ownership/cache/identity wrapper, real CSR driver,
 * with injected completion. No FFT arithmetic or hardware is claimed here.
 */
#define _GNU_SOURCE
#include "radar_dsp_diag.h"
#include "radar_crc32.h"
#include <assert.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

typedef struct {
    uint32_t r[80],starts,writes,range_invalidated,mailbox_flushes;
    uint64_t time;
} Model;
static char messages[65536];static size_t message_bytes;
u32 radar_console_free(const RadarConsole *c){(void)c;return 8192U;}
int radar_console_printf(RadarConsole *c,const char *fmt,...)
{
    va_list ap;int n;(void)c;va_start(ap,fmt);
    n=vsnprintf(messages+message_bytes,sizeof messages-message_bytes,fmt,ap);va_end(ap);
    assert(n>=0 && (size_t)n<sizeof messages-message_bytes);message_bytes+=(size_t)n;return 0;
}
static uint32_t rd(void *ctx,uint32_t off)
{ Model *m=ctx;assert(!(off&3U)&&off<320U);return m->r[off/4U]; }
static void wr(void *ctx,uint32_t off,uint32_t v)
{
    Model *m=ctx;++m->writes;
    if(off==8U) {
        if(v==4U)m->r[3]=0U;
        if(v==1U){m->r[3]=1U;++m->starts;}
        if(v==8U)m->r[3]=1U;
    } else m->r[off/4U]=v;
}
static void flush(void *ctx,uint32_t a,uint32_t n)
{
    Model *m=ctx;assert(!(a&63U)&&!(n&63U));
    if(a==(uint32_t)(uintptr_t)radar_dsp_diag_mailbox){assert(n==64U);++m->mailbox_flushes;}
}
static void inv(void *ctx,uint32_t a,uint32_t n)
{
    Model *m=ctx;assert(!(a&63U)&&!(n&63U));assert(!(m->r[3]&1U));
    if(a==RADAR_DSP_RANGE && n==RADAR_DSP_COMPLEX_BYTES)m->range_invalidated=1U;
}
static void barrier(void *ctx){(void)ctx;}
static uint64_t clock_us(void *ctx){return ((Model *)ctx)->time;}
static void init(Model *m,RadarDspDiag *d,RadarConsole *c)
{
    RadarDspIo io={m,rd,wr,flush,inv,barrier,clock_us,1};
    memset(m,0,sizeof *m);m->r[0]=RADAR_DSP_MAGIC;m->r[1]=RADAR_DSP_ABI;
    message_bytes=0U;messages[0]=0;
    radar_dsp_diag_init(d,c,&io);
}
static uint32_t crc(uint32_t a,uint32_t n)
{ return radar_crc32_update(UINT32_MAX,(const void *)(uintptr_t)a,n)^UINT32_MAX; }
static void step_until(Model *m,RadarDspDiag *d,uint32_t state)
{
    unsigned watchdog=10000U;
    while(d->state!=state && watchdog--) { m->time+=10U;radar_dsp_diag_poll(d); }
    assert(d->state==state);radar_dsp_diag_poll(d);
}
static void valid_result(Model *m)
{
    memset((void *)(uintptr_t)RADAR_DSP_RANGE,0,RADAR_DSP_COMPLEX_BYTES);
    memset((void *)(uintptr_t)RADAR_DSP_CUBE,0,RADAR_DSP_COMPLEX_BYTES);
    memset((void *)(uintptr_t)RADAR_DSP_RD,0,RADAR_DSP_RD_BYTES);
    memset((void *)(uintptr_t)RADAR_DSP_SPECTRUM,0,RADAR_DSP_SPECTRUM_BYTES);
    memset((void *)(uintptr_t)RADAR_DSP_CANDIDATES,0,RADAR_DSP_CANDIDATES_BYTES);
    m->r[3]=2U;m->r[0x78U/4U]=3456U;m->r[0x68U/4U]=123456U;
}
static void run_crc(RadarDspDiag *d,uint32_t value)
{ char command[64];snprintf(command,sizeof command,"dsp diag run %08x",value);assert(radar_dsp_diag_command(d,command,0)); }
static void check_tones(void)
{
    static const unsigned tx[3]={0U,2U,1U};
    const int32_t *p=(const int32_t *)(uintptr_t)RADAR_DSP_INPUT;
    unsigned loop,slot,rx,n,component;
    for(loop=0U;loop<32U;loop+=7U)for(slot=0U;slot<3U;++slot)for(rx=0U;rx<4U;++rx)
      for(n=0U;n<256U;n+=53U)for(component=0U;component<2U;++component) {
        unsigned physical=tx[slot]*4U+rx;
        int phase=(int)((8U+3U*physical)*n+physical*11U)+((int)(physical%7U)-3)*(int)loop*8+(component?0:64);
        int sine_value=(int)lround(32767.0*sin(2.0*3.14159265358979323846*(double)(phase&255)/256.0));
        int expected=sine_value*(int)(256U+64U*physical)/32768;
        size_t i=((((size_t)loop*3U+slot)*4U+rx)*256U+n)*2U+component;
        assert(p[i]==expected);
      }
}
static void test_contract(Model *m,RadarDspDiag *d,RadarConsole *c)
{
    RadarDspJob j={0};RadarDspDriver other;
    init(m,d,c);radar_dsp_init(&other,&d->driver.io);
    j.job_id=j.config_epoch=j.frame.frame_id=j.frame.acquisition_id=1U;j.backend=PL_BASELINE;
    j.frame.raw=(const uint8_t *)(uintptr_t)RADAR_DSP_INPUT;j.frame.bytes=RADAR_DSP_COMPLEX_BYTES;
    j.frame.flags=RADAR_FRAME_DIAGNOSTIC_PACKED|RADAR_FRAME_CRC_VALID;
    memset(j.frame.cfg_sha256,'a',64U);memset(j.frame.profile_id,'b',16U);radar_dsp_job_layout(&j);
    assert(radar_dsp_submit(&other,&j)==RADAR_DSP_E_ARGUMENT);
    j.frame.flags|=RADAR_FRAME_HSI_VALID;
    assert(radar_dsp_submit_diagnostic(&d->driver,&j)==RADAR_DSP_E_ARGUMENT);
    j.frame.flags&=~RADAR_FRAME_HSI_VALID;j.backend=PL_ENHANCED;
    assert(radar_dsp_submit_diagnostic(&d->driver,&j)==RADAR_DSP_E_ARGUMENT);
    j.backend=PL_BASELINE;j.frame.flags|=RADAR_FRAME_TIME_TRUSTED;
    assert(radar_dsp_submit_diagnostic(&d->driver,&j)==RADAR_DSP_E_ARGUMENT);
    j.frame.flags&=~RADAR_FRAME_TIME_TRUSTED;j.frame.bytes=399360U;
    assert(radar_dsp_submit_diagnostic(&d->driver,&j)==RADAR_DSP_E_ARGUMENT);
    j.frame.bytes=RADAR_DSP_COMPLEX_BYTES;
    assert(radar_dsp_submit_diagnostic(&d->driver,&j)==0);
    assert(d->driver.job.frame.flags==(RADAR_FRAME_DIAGNOSTIC_PACKED|RADAR_FRAME_CRC_VALID));
    assert(m->r[0x50/4]==(RADAR_FRAME_DIAGNOSTIC_PACKED|3U));
    assert(!other.submitted && !other.last_acquisition);
    valid_result(m);assert(radar_dsp_poll(&d->driver)==RADAR_DSP_DONE);
    assert(radar_dsp_submit_diagnostic(&d->driver,&j)==RADAR_DSP_E_IDENTITY);
    puts("PASS diagnostic input ABI adapter, no false HSI/time, baseline-only, production API separation and source dedup");
}
int main(void)
{
    Model m;RadarDspDiag d;RadarConsole c={0};uint32_t before;
    void *mapped=mmap((void *)(uintptr_t)RADAR_DSP_WORK_BASE,0x01000000U,PROT_READ|PROT_WRITE,
        MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
    assert(mapped==(void *)(uintptr_t)RADAR_DSP_WORK_BASE);
    test_contract(&m,&d,&c);
    init(&m,&d,&c);assert(!radar_dsp_diag_command(&d,"scope status",1));
    assert(radar_dsp_diag_command(&d,"dsp diag zero",0));assert(d.state==RADAR_DIAG_IDLE&&!m.starts);
    assert(radar_dsp_diag_command(&d,"dsp diag tone",1));
    step_until(&m,&d,RADAR_DIAG_RUNNING);check_tones();before=crc(RADAR_DSP_INPUT,RADAR_DSP_COMPLEX_BYTES);
    assert(m.starts==1U&&d.input_crc==before&&radar_dsp_diag_owns(&d));
    assert(radar_dsp_diag_command(&d,"dsp diag release",1));assert(d.state==RADAR_DIAG_RUNNING);
    assert(radar_dsp_diag_command(&d,"dsp diag zero",1));assert(crc(RADAR_DSP_INPUT,RADAR_DSP_COMPLEX_BYTES)==before);
    valid_result(&m);step_until(&m,&d,RADAR_DIAG_FROZEN);
    assert(m.range_invalidated&&radar_dsp_diag_mailbox[2]==2U&&radar_dsp_diag_mailbox[3]==d.token);
    assert(strstr(messages,"[DIAG REGION]")&&strstr(messages,"HSI=NOT_APPLICABLE"));
    assert(d.region_crc[0]==before&&d.region_crc[1]==crc(RADAR_DSP_RANGE,RADAR_DSP_COMPLEX_BYTES));
    assert(radar_dsp_diag_command(&d,"dsp diag release",0));assert(!radar_dsp_diag_owns(&d)&&!radar_dsp_diag_mailbox[2]);
    puts("PASS exclusive acquisition, independent channel tones, bounded prepare/check, output cache visibility and frozen receipt");

    assert(radar_dsp_diag_command(&d,"dsp diag load",1));assert(d.state==RADAR_DIAG_LOAD_READY&&radar_dsp_diag_mailbox[2]==1U);
    run_crc(&d,before^1U);assert(radar_dsp_diag_mailbox[2]==0U);
    step_until(&m,&d,RADAR_DIAG_FAILED);assert(d.error==RADAR_DIAG_E_CRC&&m.starts==1U);
    radar_dsp_diag_command(&d,"dsp diag release",1);
    radar_dsp_diag_command(&d,"dsp diag load",1);*(int32_t *)(uintptr_t)RADAR_DSP_INPUT=65536;
    run_crc(&d,crc(RADAR_DSP_INPUT,RADAR_DSP_COMPLEX_BYTES));
    step_until(&m,&d,RADAR_DIAG_FAILED);assert(d.error==RADAR_DIAG_E_FORMAT&&m.starts==1U);
    puts("PASS host load handshake revocation, CRC rejection and signed16 input format rejection before START");

    radar_dsp_diag_command(&d,"dsp diag release",1);radar_dsp_diag_command(&d,"dsp diag zero",1);
    step_until(&m,&d,RADAR_DIAG_RUNNING);valid_result(&m);
    *(uint32_t *)(uintptr_t)(RADAR_DSP_CANDIDATES+RADAR_DSP_CANDIDATES_BYTES)^=1U;
    step_until(&m,&d,RADAR_DIAG_FAILED);assert(d.error==RADAR_DIAG_E_CANARY&&!radar_dsp_diag_mailbox[2]);
    radar_dsp_diag_command(&d,"dsp diag release",1);radar_dsp_diag_command(&d,"dsp diag zero",1);
    step_until(&m,&d,RADAR_DIAG_RUNNING);valid_result(&m);*(int32_t *)(uintptr_t)RADAR_DSP_RANGE=0x5a5a5a5a;
    step_until(&m,&d,RADAR_DIAG_FAILED);assert(d.error==RADAR_DIAG_E_FORMAT);
    radar_dsp_diag_command(&d,"dsp diag release",1);radar_dsp_diag_command(&d,"dsp diag zero",1);
    step_until(&m,&d,RADAR_DIAG_RUNNING);valid_result(&m);*(int32_t *)(uintptr_t)RADAR_DSP_INPUT=123;
    step_until(&m,&d,RADAR_DIAG_FAILED);assert(d.error==RADAR_DIAG_E_CRC);
    puts("PASS boundary guard, unwritten/non-sign-extended output and changed input invalidate result");

    radar_dsp_diag_command(&d,"dsp diag release",1);radar_dsp_diag_command(&d,"dsp diag zero",1);
    step_until(&m,&d,RADAR_DIAG_RUNNING);radar_dsp_diag_cancel(&d);
    assert(d.driver.state==RADAR_DSP_CANCELING);
    radar_dsp_diag_command(&d,"dsp diag release",1);assert(radar_dsp_diag_owns(&d));
    m.r[3]=6U;m.r[4]=UINT32_C(0x80000001);step_until(&m,&d,RADAR_DIAG_FAILED);
    assert(d.driver.state==RADAR_DSP_LOCKED&&d.driver.recoverable&&!radar_dsp_diag_mailbox[2]);
    radar_dsp_diag_command(&d,"dsp diag release",1);assert(radar_dsp_diag_owns(&d));
    radar_dsp_diag_command(&d,"dsp diag recover",1);assert(d.state==RADAR_DIAG_RECOVERING);
    m.r[3]=m.r[4]=0U;step_until(&m,&d,RADAR_DIAG_STOPPED);
    radar_dsp_diag_command(&d,"dsp diag release",1);radar_dsp_diag_command(&d,"dsp diag zero",1);
    step_until(&m,&d,RADAR_DIAG_RUNNING);valid_result(&m);step_until(&m,&d,RADAR_DIAG_FROZEN);
    assert(m.mailbox_flushes>20U);
    puts("PASS cancel/drain lock retention, explicit idle recovery and subsequent normal task");
    assert(munmap(mapped,0x01000000U)==0);
    puts("PASS production diagnostic wrapper; hardware and FFT accuracy not tested by this host harness");return 0;
}
