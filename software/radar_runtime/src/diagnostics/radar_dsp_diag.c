/* SPDX-License-Identifier: MIT
 * Explicit laboratory ownership of the existing DSP workspace. No RF, HSI
 * certification, temporal enhancement, or display publication happens here.
 */
#include "radar_dsp_diag.h"
#include "radar_crc32.h"
#include <string.h>
#include <limits.h>

#define CHUNK_BYTES 4096U
#define GUARD0 (RADAR_DSP_CANDIDATES+RADAR_DSP_CANDIDATES_BYTES)
#define GUARD1 (RADAR_DSP_TEMPORAL+RADAR_DSP_TEMPORAL_BYTES)
#define GUARD_BYTES 64U
#define DIAG_CFG "15b91f7d3572343cbe45c1d716e23a72e7bcab1f2f8610a696135b11a145a401"
#define DIAG_PROFILE "15b91f7d3572343c"
#define SAY(d,...) radar_console_printf((d)->console,__VA_ARGS__)

volatile uint32_t radar_dsp_diag_mailbox[16] __attribute__((aligned(64)));
static const uint32_t addresses[RADAR_DIAG_REGIONS]={RADAR_DSP_INPUT,RADAR_DSP_RANGE,
    RADAR_DSP_CUBE,RADAR_DSP_RD,RADAR_DSP_SPECTRUM,RADAR_DSP_CANDIDATES};
static const uint32_t lengths[RADAR_DIAG_REGIONS]={RADAR_DSP_COMPLEX_BYTES,
    RADAR_DSP_COMPLEX_BYTES,RADAR_DSP_COMPLEX_BYTES,RADAR_DSP_RD_BYTES,
    RADAR_DSP_SPECTRUM_BYTES,RADAR_DSP_CANDIDATES_BYTES};
static const char *const names[RADAR_DIAG_REGIONS]={"input","range","cube","rd","spectrum","candidates"};
static const int16_t quarter_sine[65]={
    0,804,1608,2410,3212,4011,4808,5602,6393,7179,7962,8739,9512,10278,11039,11793,
    12539,13279,14010,14732,15446,16151,16846,17530,18204,18868,19519,20159,20787,
    21403,22005,22594,23170,23731,24279,24811,25329,25832,26319,26790,27245,27683,
    28105,28510,28898,29268,29621,29956,30273,30571,30852,31113,31356,31580,31785,
    31971,32137,32285,32412,32521,32609,32678,32728,32757,32767};

static const char *state_name(uint32_t s)
{
    static const char *const n[]={"IDLE","PREPARE","LOAD_READY","VERIFY",
        "OUTPUT_PREPARE","RUNNING","CHECK","FROZEN","FAILED","RECOVERING","STOPPED"};
    return s<sizeof n/sizeof n[0]?n[s]:"INVALID";
}
static const char *source_name(uint32_t s)
{ return s==RADAR_DIAG_ZERO?"ZERO":s==RADAR_DIAG_TONE?"CHANNEL_TONE":
         s==RADAR_DIAG_HOST_REPLAY?"HOST_REPLAY_PACKED":"NONE"; }
static uint64_t now(const RadarDspDiag *d)
{ return d->driver.io.now?d->driver.io.now(d->driver.io.context):0U; }
static void publish_mailbox(RadarDspDiag *d)
{
    unsigned i;uint32_t state=0U;
    if(d->state==RADAR_DIAG_LOAD_READY)state=1U;
    if(d->state==RADAR_DIAG_FROZEN)state=2U;
    radar_dsp_diag_mailbox[2]=0U;
    radar_dsp_diag_mailbox[0]=UINT32_C(0x44494147);radar_dsp_diag_mailbox[1]=1U;
    radar_dsp_diag_mailbox[3]=d->token;radar_dsp_diag_mailbox[4]=RADAR_DSP_INPUT;
    radar_dsp_diag_mailbox[5]=RADAR_DSP_COMPLEX_BYTES;
    radar_dsp_diag_mailbox[6]=d->input_crc;radar_dsp_diag_mailbox[7]=d->driver.job.job_id;
    radar_dsp_diag_mailbox[8]=(uint32_t)d->error;radar_dsp_diag_mailbox[9]=d->source;
    radar_dsp_diag_mailbox[10]=(uint32_t)d->driver.state;
    for(i=11U;i<16U;++i)radar_dsp_diag_mailbox[i]=0U;
    radar_dsp_diag_mailbox[2]=state;
    if(d->driver.io.flush)d->driver.io.flush(d->driver.io.context,
        (uint32_t)(uintptr_t)radar_dsp_diag_mailbox,64U);
    if(d->driver.io.barrier)d->driver.io.barrier(d->driver.io.context);
}
static void set_state(RadarDspDiag *d,uint32_t s)
{ d->state=s;publish_mailbox(d);d->report_pending=1; }
static void fail(RadarDspDiag *d,int error)
{ d->error=error;set_state(d,RADAR_DIAG_FAILED); }
static void report(RadarDspDiag *d)
{
    unsigned i;
    if(!d->report_pending || !d->console || radar_console_free(d->console)<2400U)return;
    SAY(d,"[DIAG STATE] state=%s token=%u source=%s driver=%u error=%d valid=%u HSI=NOT_APPLICABLE RF=OFFLINE_REPLAY\r\n",
        state_name(d->state),(unsigned)d->token,source_name(d->source),(unsigned)d->driver.state,
        d->error,(unsigned)(d->state==RADAR_DIAG_FROZEN));
    if(d->state==RADAR_DIAG_LOAD_READY)
        SAY(d,"[DIAG LOAD_READY] token=%u input_addr=%08x bytes=%u source=HOST_REPLAY_PACKED\r\n",
            (unsigned)d->token,(unsigned)RADAR_DSP_INPUT,(unsigned)RADAR_DSP_COMPLEX_BYTES);
    if(d->state==RADAR_DIAG_FROZEN) {
        SAY(d,"[DIAG FROZEN] token=%u job=%u source=%s input_crc=%08x cycles=%llu wall_us=%llu vectors=%u candidates=%u\r\n",
            (unsigned)d->token,(unsigned)d->driver.result.job_id,source_name(d->source),
            (unsigned)d->input_crc,(unsigned long long)d->driver.result.cycles,
            (unsigned long long)(d->done_us-d->submitted_us),(unsigned)d->driver.result.vector_count,
            (unsigned)d->driver.result.candidate_count);
        for(i=0U;i<RADAR_DIAG_REGIONS;++i)
            SAY(d,"[DIAG REGION] token=%u name=%s addr=%08x bytes=%u crc=%08x\r\n",
                (unsigned)d->token,names[i],(unsigned)addresses[i],(unsigned)lengths[i],(unsigned)d->region_crc[i]);
    }
    d->report_pending=0;
}
static int parse_crc(const char *s,uint32_t *out)
{
    unsigned i;uint32_t n=0U;
    for(i=0U;i<8U;++i) {
        unsigned v;char c=s[i];
        if(c>='0'&&c<='9')v=(unsigned)(c-'0');
        else if(c>='a'&&c<='f')v=(unsigned)(c-'a')+10U;
        else if(c>='A'&&c<='F')v=(unsigned)(c-'A')+10U;
        else return 0;
        n=(n<<4)|v;
    }
    if(s[8])return 0;
    *out=n;return 1;
}
static int32_t sine(unsigned phase)
{
    unsigned q=(phase>>6)&3U,i=phase&63U;
    int32_t a=quarter_sine[(q&1U)?64U-i:i];return (q&2U)?-a:a;
}
static void generate(RadarDspDiag *d,uint32_t count)
{
    static const unsigned tx[3]={0U,2U,1U};
    int32_t *p=(int32_t *)(uintptr_t)(RADAR_DSP_INPUT+d->offset);
    unsigned k;
    if(d->source==RADAR_DIAG_ZERO){memset(p,0,count);return;}
    for(k=0U;k<count/8U;++k) {
        const unsigned sample_index=d->offset/8U+k,sample=sample_index&255U;
        const unsigned vector=sample_index>>8,chirp=vector>>2,rx=vector&3U;
        const unsigned physical=tx[chirp%3U]*4U+rx,loop=chirp/3U;
        const int phase=(int)((8U+3U*physical)*sample+physical*11U)+((int)(physical%7U)-3)*(int)loop*8;
        const int32_t amplitude=(int32_t)(256U+64U*physical);
        /* Exact deterministic signed division toward zero. Input remains
         * signed16 in signed32 slots; no floating point generator involved. */
        p[k*2U]=sine((unsigned)phase+64U)*amplitude/32768;
        p[k*2U+1U]=sine((unsigned)phase)*amplitude/32768;
    }
}
static void guards(RadarDspDiag *d,int prepare)
{
    unsigned i,k;const uint32_t a[2]={GUARD0,GUARD1};
    const uint32_t value=UINT32_C(0xa55aa55a)^d->token;
    for(k=0U;k<2U;++k) {
        uint32_t *p=(uint32_t *)(uintptr_t)a[k];
        if(prepare) {
            for(i=0U;i<16U;++i)p[i]=value;
            d->driver.io.flush(d->driver.io.context,a[k],GUARD_BYTES);
        } else {
            d->driver.io.invalidate(d->driver.io.context,a[k],GUARD_BYTES);
            d->driver.io.barrier(d->driver.io.context);
            for(i=0U;i<16U;++i)if(p[i]!=value){fail(d,RADAR_DIAG_E_CANARY);return;}
        }
    }
}
static void submit_job(RadarDspDiag *d)
{
    RadarDspJob j={0};int rc;
    j.backend=PL_BASELINE;j.job_id=d->token;j.config_epoch=1U;
    j.frame.raw=(const uint8_t *)(uintptr_t)RADAR_DSP_INPUT;
    j.frame.bytes=RADAR_DSP_COMPLEX_BYTES;j.frame.frame_id=d->token;
    j.frame.acquisition_id=d->token;j.frame.crc32=d->input_crc;
    j.frame.flags=RADAR_FRAME_DIAGNOSTIC_PACKED|RADAR_FRAME_CRC_VALID;
    memcpy(j.frame.cfg_sha256,DIAG_CFG,sizeof DIAG_CFG);
    memcpy(j.frame.profile_id,DIAG_PROFILE,sizeof DIAG_PROFILE);
    radar_dsp_job_layout(&j);guards(d,1);
    d->submitted_us=now(d);rc=radar_dsp_submit_diagnostic(&d->driver,&j);
    if(rc){fail(d,rc);return;}set_state(d,RADAR_DIAG_RUNNING);
}
static void begin(RadarDspDiag *d,uint32_t source)
{
    int rc;
    if(d->token==UINT32_MAX){fail(d,RADAR_DSP_E_IDENTITY);return;}
    rc=radar_dsp_probe(&d->driver);if(rc){fail(d,rc);return;}
    ++d->token;d->source=source;d->offset=0U;d->region=0U;d->error=0;
    d->input_crc=0U;d->crc=UINT32_MAX;d->expected_crc_valid=0;
    d->submitted_us=d->done_us=0U;memset(d->region_crc,0,sizeof d->region_crc);
    if(source==RADAR_DIAG_HOST_REPLAY) {
        /* Clean and evict all possible CPU writes BEFORE the host owns this
         * range. There are no CPU touches until explicit run/release. */
        d->driver.io.flush(d->driver.io.context,RADAR_DSP_INPUT,RADAR_DSP_COMPLEX_BYTES);
        d->driver.io.invalidate(d->driver.io.context,RADAR_DSP_INPUT,RADAR_DSP_COMPLEX_BYTES);
        d->driver.io.barrier(d->driver.io.context);set_state(d,RADAR_DIAG_LOAD_READY);
    } else set_state(d,RADAR_DIAG_PREPARE);
}
void radar_dsp_diag_init(RadarDspDiag *d,RadarConsole *c,const RadarDspIo *io)
{ memset(d,0,sizeof *d);d->console=c;radar_dsp_init(&d->driver,io);publish_mailbox(d); }
int radar_dsp_diag_owns(const RadarDspDiag *d)
{ return d && d->state!=RADAR_DIAG_IDLE; }
void radar_dsp_diag_cancel(RadarDspDiag *d)
{
    if(!d || d->state==RADAR_DIAG_IDLE)return;
    if(d->state==RADAR_DIAG_RUNNING) {
        int rc=radar_dsp_cancel(&d->driver);if(rc){fail(d,rc);return;}
        publish_mailbox(d);d->report_pending=1;return;
    }
    if(radar_dsp_busy(&d->driver))return;
    d->error=RADAR_DSP_E_CANCELED;set_state(d,RADAR_DIAG_STOPPED);
}
int radar_dsp_diag_command(RadarDspDiag *d,const char *cmd,int may_acquire)
{
    uint32_t expected;int rc;
    if(strncmp(cmd,"dsp diag",8U) || (cmd[8] && cmd[8]!=' '))return 0;
    if(!strcmp(cmd,"dsp diag") || !strcmp(cmd,"dsp diag status")){d->report_pending=1;report(d);return 1;}
    if(!strcmp(cmd,"dsp diag cancel")){radar_dsp_diag_cancel(d);return 1;}
    if(!strcmp(cmd,"dsp diag recover")) {
        if(!radar_dsp_diag_owns(d)){SAY(d,"[DIAG REJECT] no diagnostic ownership\r\n");return 1;}
        rc=radar_dsp_recover(&d->driver);
        if(rc){SAY(d,"[DIAG REJECT] recover=%d; no busy reset\r\n",rc);return 1;}
        set_state(d,RADAR_DIAG_RECOVERING);return 1;
    }
    if(!strcmp(cmd,"dsp diag release")) {
        if(radar_dsp_busy(&d->driver)){SAY(d,"[DIAG REJECT] driver not quiescent\r\n");return 1;}
        set_state(d,RADAR_DIAG_IDLE);return 1;
    }
    if(!strncmp(cmd,"dsp diag run ",13U)) {
        if(d->state!=RADAR_DIAG_LOAD_READY || !parse_crc(cmd+13,&expected)) {
            SAY(d,"[DIAG REJECT] run needs LOAD_READY and exactly eight CRC hex digits\r\n");return 1;
        }
        /* Revoke host access before any CPU or PL consumption. */
        d->expected_crc=expected;d->expected_crc_valid=1;d->offset=0U;d->crc=UINT32_MAX;
        set_state(d,RADAR_DIAG_VERIFY);
        d->driver.io.invalidate(d->driver.io.context,RADAR_DSP_INPUT,RADAR_DSP_COMPLEX_BYTES);
        d->driver.io.barrier(d->driver.io.context);return 1;
    }
    if(!strcmp(cmd,"dsp diag zero") || !strcmp(cmd,"dsp diag tone") || !strcmp(cmd,"dsp diag load")) {
        if(!may_acquire || radar_dsp_diag_owns(d)) {
            SAY(d,"[DIAG REJECT] require exclusive idle RF/capture/scan/CLI ownership; release old diagnostic first\r\n");return 1;
        }
        begin(d,!strcmp(cmd,"dsp diag zero")?RADAR_DIAG_ZERO:
            !strcmp(cmd,"dsp diag tone")?RADAR_DIAG_TONE:RADAR_DIAG_HOST_REPLAY);return 1;
    }
    SAY(d,"[DIAG] dsp diag zero|tone|load|run CRC32|status|cancel|recover|release\r\n");return 1;
}
void radar_dsp_diag_poll(RadarDspDiag *d)
{
    uint32_t count,i;const int32_t *words;int rc;
    report(d);
    if(d->state==RADAR_DIAG_PREPARE) {
        count=RADAR_DSP_COMPLEX_BYTES-d->offset;if(count>CHUNK_BYTES)count=CHUNK_BYTES;
        generate(d,count);d->offset+=count;
        if(d->offset==RADAR_DSP_COMPLEX_BYTES){d->offset=0U;d->crc=UINT32_MAX;set_state(d,RADAR_DIAG_VERIFY);}
    } else if(d->state==RADAR_DIAG_VERIFY) {
        count=RADAR_DSP_COMPLEX_BYTES-d->offset;if(count>CHUNK_BYTES)count=CHUNK_BYTES;
        words=(const int32_t *)(uintptr_t)(RADAR_DSP_INPUT+d->offset);
        for(i=0U;i<count/4U;++i)if(words[i]<-32768 || words[i]>32767){fail(d,RADAR_DIAG_E_FORMAT);return;}
        d->crc=radar_crc32_update(d->crc,words,count);d->offset+=count;
        if(d->offset==RADAR_DSP_COMPLEX_BYTES) {
            d->input_crc=d->crc^UINT32_MAX;
            if(d->expected_crc_valid && d->input_crc!=d->expected_crc){fail(d,RADAR_DIAG_E_CRC);return;}
            d->offset=0U;d->region=1U;set_state(d,RADAR_DIAG_OUTPUT_PREPARE);
        }
    } else if(d->state==RADAR_DIAG_OUTPUT_PREPARE) {
        count=lengths[d->region]-d->offset;if(count>CHUNK_BYTES)count=CHUNK_BYTES;
        /* Range/cube poison is not valid sign-extended signed24. An unwritten
         * element therefore fails the explicit output-format check. Unused
         * candidate capacity is initialized to zero for reproducible export. */
        memset((void *)(uintptr_t)(addresses[d->region]+d->offset),d->region==5U?0:0x5a,count);
        d->offset+=count;
        if(d->offset==lengths[d->region]) {
            d->offset=0U;++d->region;
            if(d->region==RADAR_DIAG_REGIONS)submit_job(d);
        }
    } else if(d->state==RADAR_DIAG_RUNNING) {
        rc=radar_dsp_poll(&d->driver);if(rc==RADAR_DSP_PENDING)return;
        if(rc==RADAR_DSP_E_CANCELED){d->error=rc;set_state(d,RADAR_DIAG_STOPPED);return;}
        if(rc!=RADAR_DSP_DONE){fail(d,rc);return;}
        d->done_us=now(d);
        d->driver.io.invalidate(d->driver.io.context,RADAR_DSP_RANGE,RADAR_DSP_COMPLEX_BYTES);
        d->driver.io.invalidate(d->driver.io.context,RADAR_DSP_INPUT,RADAR_DSP_COMPLEX_BYTES);
        d->driver.io.barrier(d->driver.io.context);guards(d,0);
        if(d->state==RADAR_DIAG_FAILED)return;
        {
            const RadarDspCandidateHeader *h=(const RadarDspCandidateHeader *)(uintptr_t)RADAR_DSP_CANDIDATES;
            if(h->error || h->count!=d->driver.result.candidate_count || h->total<h->count ||
               h->truncated!=h->total-h->count){fail(d,RADAR_DIAG_E_RESULT);return;}
        }
        d->region=0U;d->offset=0U;d->crc=UINT32_MAX;set_state(d,RADAR_DIAG_CHECK);
    } else if(d->state==RADAR_DIAG_CHECK) {
        count=lengths[d->region]-d->offset;if(count>CHUNK_BYTES)count=CHUNK_BYTES;
        words=(const int32_t *)(uintptr_t)(addresses[d->region]+d->offset);
        if(d->region==1U || d->region==2U)
            for(i=0U;i<count/4U;++i)if(words[i]<-8388608 || words[i]>8388607){fail(d,RADAR_DIAG_E_FORMAT);return;}
        d->crc=radar_crc32_update(d->crc,words,count);d->offset+=count;
        if(d->offset==lengths[d->region]) {
            d->region_crc[d->region]=d->crc^UINT32_MAX;
            if(!d->region && d->region_crc[0]!=d->input_crc){fail(d,RADAR_DIAG_E_CRC);return;}
            d->offset=0U;d->crc=UINT32_MAX;++d->region;
            if(d->region==RADAR_DIAG_REGIONS)set_state(d,RADAR_DIAG_FROZEN);
        }
    } else if(d->state==RADAR_DIAG_RECOVERING) {
        rc=radar_dsp_poll(&d->driver);if(rc==RADAR_DSP_PENDING)return;
        if(rc!=RADAR_DSP_DONE || d->driver.state!=RADAR_DSP_IDLE){fail(d,rc);return;}
        SAY(d,"[DIAG RECOVERED] token=%u first_error=%d; result invalid, release before next task\r\n",
            (unsigned)d->token,d->error);set_state(d,RADAR_DIAG_STOPPED);
    }
}
