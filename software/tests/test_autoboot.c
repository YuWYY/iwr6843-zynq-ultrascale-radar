/* SPDX-License-Identifier: MIT
 * Production autoboot FSM with hardware/CLI service seams. These tests prove
 * sequencing, bounded wait, state-based admission and cancellation, not boot
 * media, voltages, RF, DP training or physical display operation.
 */
#include "radar_autoboot.h"
#include "radar_display.h"
#include <assert.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
static RadarAutoboot boot;
static RadarScan scan;
static RadarStreamScan stream;
static RadarSession session;
static RadarSw2 sw2;
static RadarSw3 sw3;
static RadarConsole console;
static RadarCaptureProfile profile;
static u64 clock_us;
static int connected,display_on,display_ack,display_fault,ready,block_preflight;
static int fail_backend,fail_mode,fail_profile,fail_io,hang_dma,fail_connect,stale_check;
static int fail_display,fail_live;
static unsigned train_calls,live_calls,stop_calls,dma_calls,check_calls,reset_calls,safe_calls,stream_safe_calls;
static u32 fifo_observations;
static char calls[512];
static void note(char c){size_t n=strlen(calls);assert(n+1U<sizeof calls);calls[n]=c;calls[n+1U]=0;}
u64 radar_hw_now_us(void){return clock_us;}
int radar_console_printf(RadarConsole *c,const char *fmt,...)
{char text[512];int n;va_list args;(void)c;va_start(args,fmt);n=vsnprintf(text,sizeof text,fmt,args);va_end(args);assert(n>=0 && n<(int)sizeof text);return n;}
int radar_scan_command(RadarScan *s,const char *cmd)
{
    if(!strcmp(cmd,"scope backend PL_BASELINE")){note('B');if(!fail_backend)s->backend=PL_BASELINE;}
    else if(!strcmp(cmd,"scope display decoupled")){note('D');s->display_decoupled=1U;}
    else assert(0);
    return 1;
}
int radar_stream_scan_command(RadarStreamScan *p,const char *cmd)
{
    if(!strcmp(cmd,"capture mode stream")){note('M');if(!fail_mode)p->mode=1U;}
    else if(!strcmp(cmd,"capture stream continuous")){note('P');p->frame_limit=0U;profile.profile_id=fail_profile?"wrong":RADAR_AUTOBOOT_PROFILE;}
    else if(!strcmp(cmd,"scope live")){note('L');++live_calls;if(!fail_live){p->running=1U;p->capture.active=1U;}}
    else if(!strcmp(cmd,"scope stop")){note('S');++stop_calls;p->stop_requested=1U;}
    else if(!strcmp(cmd,"safe")){note('E');++stream_safe_calls;p->stop_requested=1U;}
    else assert(0);
    return 1;
}
int radar_stream_active(const RadarStream *r){return r->active;}
void radar_session_cancel(RadarSession *s){(void)s;note('C');}
int radar_sw3_command(RadarSw3 *s,const char *cmd)
{
    if(!strcmp(cmd,"dma qualify")){note('Q');++dma_calls;s->job=SW3_WAIT_RESET;}
    else if(!strcmp(cmd,"safe")){note('X');s->job=SW3_IDLE;ready=0;}
    else assert(0);
    return 1;
}
int radar_sw3_capture_ready(RadarSw3 *s){(void)s;return ready;}
void radar_sw2_command(RadarSw2 *s,const char *cmd)
{
    if(!strcmp(cmd,"confirm io")){note('I');if(!fail_io)s->wiring_confirmed=1U;}
    else if(!strcmp(cmd,"connect")){note('U');if(!fail_connect)s->state=SW2_CONNECT_WAIT;}
    else if(!strcmp(cmd,"check10")){note('T');++check_calls;s->job=SW2_JOB_CHECK10;s->state=SW2_QUERY;}
    else if(!strcmp(cmd,"reset3")){note('R');++reset_calls;s->job=SW2_JOB_RESET3;s->state=SW2_RESET_HOLD;}
    else if(!strcmp(cmd,"safe")){note('Z');++safe_calls;s->state=SW2_SAFE;s->job=SW2_JOB_NONE;}
    else assert(0);
}
int radar_capture_profile_validate(const RadarCaptureProfile *p){return p!=&profile;}
int radar_session_command(RadarSession *s,const char *cmd)
{assert(!strcmp(cmd,"capture confirm " RADAR_AUTOBOOT_PROFILE));note('F');s->confirmed=1U;return 1;}
u32 radar_session_preflight(RadarSession *s)
{note('A');return block_preflight || !ready || !s->confirmed || !sw2.check10_passes || !sw2.reset3_passes;}
void radar_session_print_preflight(RadarSession *s){(void)s;}
int radar_display_monitor_present(void){return connected;}
int radar_display_active(void){return display_on;}
int radar_display_ready(void){return display_on && display_ack && !(display_fault&7);}
u32 radar_display_faults(void){return display_fault;}
u32 radar_display_fifo_observations(void){return fifo_observations;}
int radar_display_command(const char *cmd,int busy)
{assert(!strcmp(cmd,"display init") && !busy);note('V');++train_calls;if(!fail_display)display_on=1;return 1;}
static RadarViewPage view;
void radar_display_request_page(RadarViewPage p){note('W');view=p;}
RadarViewPage radar_display_get_requested_page(void){return view;}
static void setup(int enabled)
{
    memset(&scan,0,sizeof scan);memset(&stream,0,sizeof stream);
    memset(&session,0,sizeof session);memset(&sw2,0,sizeof sw2);memset(&sw3,0,sizeof sw3);
    memset(&profile,0,sizeof profile);memset(calls,0,sizeof calls);
    scan.session=&session;stream.scan=&scan;session.sw2=&sw2;session.sw3=&sw3;session.profile=&profile;
    connected=1;display_on=display_fault=ready=block_preflight=0;display_ack=1;
    fail_backend=fail_mode=fail_profile=fail_io=hang_dma=fail_connect=stale_check=fail_display=fail_live=0;
    train_calls=live_calls=stop_calls=dma_calls=check_calls=reset_calls=safe_calls=0;
    stream_safe_calls=0U;
    fifo_observations=0U;
    view=RADAR_VIEW_OVERVIEW;clock_us=100U;
    radar_autoboot_init(&boot,&scan,&stream,&console,enabled);
}
static void step(void)
{
    clock_us+=10000U;
    /* Completion is supplied by the separately-polled native services, not
     * by command return values or fake pass counters in the production FSM. */
    if(sw3.job!=SW3_IDLE && !hang_dma){sw3.job=SW3_IDLE;ready=1;}
    if(sw2.state==SW2_CONNECT_WAIT)sw2.state=SW2_ONLINE;
    if(sw2.job!=SW2_JOB_NONE) {
        if(sw2.job==SW2_JOB_CHECK10 && !stale_check)++sw2.check10_passes;
        if(sw2.job==SW2_JOB_RESET3)++sw2.reset3_passes;
        sw2.job=SW2_JOB_NONE;sw2.state=SW2_ONLINE;
    }
    radar_autoboot_poll(&boot);
}
static void reach(RadarAutobootStage stage)
{unsigned i;for(i=0;i<10000 && boot.stage!=stage && radar_autoboot_active(&boot);++i)step();assert(boot.stage==stage);}
static void inert(void)
{unsigned n=(unsigned)strlen(calls);for(unsigned i=0;i<50;++i)step();assert(strlen(calls)==n);}
int main(void)
{
    setup(0);inert();assert(!radar_autoboot_active(&boot) && !live_calls);
    setup(1);reach(RADAR_BOOT_FIRST_FRAME);
    assert(!strcmp(calls,"BDMPIQUTRFAVWAL"));
    assert(train_calls==1 && dma_calls==1 && check_calls==1 && reset_calls==1 && live_calls==1);
    assert(view==RADAR_VIEW_TOP);
    stream.capture.frames_verified=1;step();assert(boot.stage==RADAR_BOOT_FIRST_FRAME);
    stream.completed=1;step();assert(boot.stage==RADAR_BOOT_FIRST_FRAME);
    stream.presented=1;step();assert(boot.stage==RADAR_BOOT_DONE);inert();
    setup(1);fail_backend=1;reach(RADAR_BOOT_FAILED);assert(!dma_calls && !live_calls);inert();
    setup(1);fail_mode=1;reach(RADAR_BOOT_FAILED);assert(!dma_calls && !live_calls);
    setup(1);fail_profile=1;reach(RADAR_BOOT_FAILED);assert(!dma_calls && !sw2.wiring_confirmed);
    setup(1);fail_io=1;reach(RADAR_BOOT_FAILED);assert(!dma_calls);
    setup(1);hang_dma=1;reach(RADAR_BOOT_FAILED);assert(dma_calls==1 && !train_calls && boot.error==-702);inert();
    setup(1);fail_connect=1;reach(RADAR_BOOT_FAILED);assert(!check_calls);
    setup(1);stale_check=1;sw2.check10_passes=9U;reach(RADAR_BOOT_FAILED);assert(!reset_calls && !live_calls);
    setup(1);reach(RADAR_BOOT_CHECK10);assert(boot.timeout_us==UINT64_C(40000000));
    step();clock_us+=UINT64_C(35000000);step();assert(boot.stage==RADAR_BOOT_RESET3);
    setup(1);block_preflight=1;reach(RADAR_BOOT_FAILED);assert(!train_calls && !live_calls);
    setup(1);connected=0;reach(RADAR_BOOT_FAILED);assert(boot.failed_stage==RADAR_BOOT_HPD && !train_calls && !live_calls);inert();
    setup(1);reach(RADAR_BOOT_HPD);for(unsigned i=0;i<80;++i){connected=i%2U;step();}assert(!train_calls);
    connected=1;reach(RADAR_BOOT_DP_ACK);assert(train_calls==1);
    setup(1);fail_display=1;reach(RADAR_BOOT_FAILED);assert(train_calls==1 && !live_calls);inert();
    setup(1);display_ack=0;reach(RADAR_BOOT_FAILED);assert(boot.failed_stage==RADAR_BOOT_DP_ACK && !live_calls);
    setup(1);reach(RADAR_BOOT_DP_ACK);display_on=0;step();assert(boot.stage==RADAR_BOOT_FAILED && !live_calls);
    setup(1);fail_live=1;reach(RADAR_BOOT_FAILED);assert(live_calls==1 && !stop_calls);inert();
    setup(1);reach(RADAR_BOOT_FIRST_FRAME);display_fault=1;step();assert(boot.stage==RADAR_BOOT_FAILED && stop_calls==1);inert();
    setup(1);display_fault=8;fifo_observations=1U;reach(RADAR_BOOT_FIRST_FRAME);
    assert(boot.dp_startup_flags==8U && boot.dp_observations==1U);
    stream.capture.frames_verified=stream.completed=stream.presented=1U;step();assert(boot.stage==RADAR_BOOT_DONE);
    ++fifo_observations;step();assert(boot.stage==RADAR_BOOT_FAILED && boot.error==-722 && stop_calls==1);inert();
    setup(1);display_fault=8;reach(RADAR_BOOT_DP_ACK);step();++fifo_observations;step();
    assert(boot.stage==RADAR_BOOT_FAILED && !live_calls);
    setup(1);reach(RADAR_BOOT_FIRST_FRAME);reach(RADAR_BOOT_FAILED);assert(boot.error==-702 && stop_calls==1);inert();
    setup(1);assert(!radar_autoboot_command(&boot,"status"));assert(!radar_autoboot_command(&boot,"capture preflight"));
    assert(radar_autoboot_command(&boot,"autoboot status"));assert(radar_autoboot_active(&boot));
    assert(!radar_autoboot_command(&boot,"capture mode finite"));assert(boot.stage==RADAR_BOOT_CANCELED);inert();
    setup(1);reach(RADAR_BOOT_DMA);step();assert(radar_autoboot_command(&boot,"safe"));assert(boot.stage==RADAR_BOOT_CANCELED && safe_calls==1);inert();
    setup(1);reach(RADAR_BOOT_FIRST_FRAME);assert(radar_autoboot_command(&boot,"scope stop"));assert(stop_calls==1);inert();
    assert(!stream_safe_calls);
    setup(1);reach(RADAR_BOOT_FIRST_FRAME);assert(radar_autoboot_command(&boot,"safe"));
    assert(boot.stage==RADAR_BOOT_CANCELED && stream_safe_calls==1 && !stop_calls && !safe_calls);inert();
    setup(1);reach(RADAR_BOOT_FIRST_FRAME);assert(radar_autoboot_command(&boot,"disable"));
    assert(boot.stage==RADAR_BOOT_CANCELED && stream_safe_calls==1 && !stop_calls && !safe_calls);inert();
    setup(1);reach(RADAR_BOOT_FIRST_FRAME);
    stream.capture.frames_verified=stream.completed=stream.presented=1U;step();assert(boot.stage==RADAR_BOOT_DONE);
    assert(!radar_autoboot_command(&boot,"scope status") && boot.dp_baseline_valid);
    assert(radar_autoboot_command(&boot,"autoboot status") && boot.dp_baseline_valid);
    assert(!radar_autoboot_command(&boot,"scope stop") && !boot.dp_baseline_valid);
    (void)radar_stream_scan_command(&stream,"scope stop");stream.running=0U;
    assert(!radar_autoboot_command(&boot,"display off"));display_on=0;
    assert(!radar_autoboot_command(&boot,"display init"));(void)radar_display_command("display init",0);
    ++fifo_observations;display_fault=8;
    assert(!radar_autoboot_command(&boot,"scope live"));(void)radar_stream_scan_command(&stream,"scope live");
    stream.stop_requested=0U;step();assert(boot.stage==RADAR_BOOT_DONE && !boot.error && stop_calls==1);
    ++fifo_observations;step();assert(boot.stage==RADAR_BOOT_DONE && stop_calls==1 && !stream_safe_calls);
    setup(1);clock_us=0;radar_autoboot_poll(&boot);assert(boot.stage==RADAR_BOOT_FAILED && boot.error==-701);
    puts("PASS autoboot: sequence, fresh native qualification, profile/readback rejection, no-monitor timeout, single DP attempt/real ACK, no fake first frame, cancellation, serial queries, no retries; hardware boot not proven");
    return 0;
}
