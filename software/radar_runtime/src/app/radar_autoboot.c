/* SPDX-License-Identifier: MIT
 * One bounded cold-boot attempt for the reviewed fixed installation.
 * Existing command handlers own all hardware operations and qualifications.
 * A handled command is NOT success: every step checks actual resulting state.
 */
#include "radar_autoboot.h"
#include "radar_display.h"
#include "radar_hw.h"
#include <string.h>
#define SAY(b,...) radar_console_printf((b)->console,__VA_ARGS__)
#define SECOND UINT64_C(1000000)
const char *radar_autoboot_stage_name(RadarAutobootStage stage)
{
    static const char *const names[]={"MANUAL_SAFE","SETTLE","PL_BASELINE","DECOUPLED",
        "STREAM_MODE","PROFILE","IO_CONTRACT","DMA_QUALIFY","CONNECT","CHECK10",
        "RESET3","CONFIRM","PREFLIGHT","WAIT_HPD","DISPLAY_INIT","DP_ACK",
        "TOP_VIEW","LIVE_START","FIRST_FRAME","LIVE_READY","FAILED","CANCELED"};
    return (unsigned)stage<sizeof names/sizeof names[0]?names[stage]:"INVALID";
}
int radar_autoboot_active(const RadarAutoboot *b)
{return b->stage>=RADAR_BOOT_SETTLE && b->stage<=RADAR_BOOT_FIRST_FRAME;}
static void enter(RadarAutoboot *b,RadarAutobootStage stage,u64 timeout)
{
    b->stage=stage;b->stage_us=radar_hw_now_us();b->timeout_us=timeout;
    b->issued=0U;b->have_stable=0U;
    SAY(b,"[AUTOBOOT] stage=%s\r\n",radar_autoboot_stage_name(stage));
}
static void stop_mode(RadarAutoboot *b,int error,int canceled,int emergency)
{
    RadarSession *s=b->scan->session;
    b->failed_stage=b->stage;b->error=error;
    b->stage=canceled?RADAR_BOOT_CANCELED:RADAR_BOOT_FAILED;
    /* Preserve DMA/RAW leases and complete native teardown in the main loop.
     * No speculative reset or restart and no blocking wait for cleanup. */
    if(b->stream->running || radar_stream_active(&b->stream->capture))
        (void)radar_stream_scan_command(b->stream,emergency?"safe":"scope stop");
    else if(s->owns)radar_session_cancel(s);
    else {
        (void)radar_sw3_command(s->sw3,"safe");
        radar_sw2_command(s->sw2,"safe");
    }
    SAY(b,"[AUTOBOOT %s] stage=%s rc=%d; no retry. Serial status/recovery remains available.\r\n",
        canceled?"CANCELED":"FAIL",radar_autoboot_stage_name(b->failed_stage),error);
}
static void stop(RadarAutoboot *b,int error,int canceled)
{stop_mode(b,error,canceled,0);}
void radar_autoboot_init(RadarAutoboot *b,RadarScan *scan,RadarStreamScan *stream,
                         RadarConsole *console,int enabled)
{
    memset(b,0,sizeof *b);b->scan=scan;b->stream=stream;b->console=console;
    b->began_us=b->last_us=radar_hw_now_us();
    if(!enabled){SAY(b,"[AUTOBOOT] disabled; original manual SAFE startup.\r\n");return;}
    SAY(b,"[AUTOBOOT] Authorized fixed-installation RF startup; profile=%s, PL_BASELINE, TOP.\r\n",RADAR_AUTOBOOT_PROFILE);
    SAY(b,"[AUTOBOOT] Confirmed electrical contract is deployment evidence, not measured voltage. safe cancels; autoboot status reports progress.\r\n");
    enter(b,RADAR_BOOT_SETTLE,3U*SECOND);
}
void radar_autoboot_poll(RadarAutoboot *b)
{
    RadarSession *s=b->scan->session;RadarSw2 *w=s->sw2;RadarSw3 *d=s->sw3;
    u64 now=radar_hw_now_us();
    /* Keep historical startup bit8 visible, but do not excuse observations
     * added after the first descriptor ACK. This ISR counter is conservative:
     * it counts nonzero observations, not uniquely identified FIFO events. */
    if(b->dp_baseline_valid &&
       ((b->stage>=RADAR_BOOT_VIEW && b->stage<=RADAR_BOOT_FIRST_FRAME) ||
        (b->stage==RADAR_BOOT_DONE && b->stream->running && !b->stream->stop_requested)) &&
       ((radar_display_faults()&7U) ||
        radar_display_fifo_observations()!=b->dp_observations)) {
        stop(b,-722,0);return;
    }
    if(!radar_autoboot_active(b))return;
    if(now<b->last_us){stop(b,-701,0);return;}
    b->last_us=now;
    if(now-b->stage_us>b->timeout_us){stop(b,-702,0);return;}
    if(w->fatal_control || w->state==SW2_FAULT || b->stream->error || s->error) {
        stop(b,-703,0);return;
    }
    switch(b->stage) {
    case RADAR_BOOT_SETTLE:
        if(now-b->stage_us>=2U*SECOND)enter(b,RADAR_BOOT_BACKEND,SECOND);
        break;
    case RADAR_BOOT_BACKEND:
        (void)radar_scan_command(b->scan,"scope backend PL_BASELINE");
        if(b->scan->backend!=PL_BASELINE){stop(b,-704,0);return;}
        enter(b,RADAR_BOOT_DECOUPLE,SECOND);break;
    case RADAR_BOOT_DECOUPLE:
        (void)radar_scan_command(b->scan,"scope display decoupled");
        if(!b->scan->display_decoupled){stop(b,-705,0);return;}
        enter(b,RADAR_BOOT_MODE,SECOND);break;
    case RADAR_BOOT_MODE:
        (void)radar_stream_scan_command(b->stream,"capture mode stream");
        if(!b->stream->mode){stop(b,-706,0);return;}
        enter(b,RADAR_BOOT_PROFILE,SECOND);break;
    case RADAR_BOOT_PROFILE:
        (void)radar_stream_scan_command(b->stream,"capture stream continuous");
        if(b->stream->frame_limit || !s->profile ||
           strcmp(s->profile->profile_id,RADAR_AUTOBOOT_PROFILE) ||
           radar_capture_profile_validate(s->profile)) {stop(b,-707,0);return;}
        enter(b,RADAR_BOOT_IO,SECOND);break;
    case RADAR_BOOT_IO:
        radar_sw2_command(w,"confirm io");
        if(!w->wiring_confirmed){stop(b,-708,0);return;}
        enter(b,RADAR_BOOT_DMA,10U*SECOND);break;
    case RADAR_BOOT_DMA:
        if(!b->issued){b->issued=1U;(void)radar_sw3_command(d,"dma qualify");}
        if(d->job==SW3_IDLE) {
            if(!radar_sw3_capture_ready(d)){stop(b,-709,0);return;}
            enter(b,RADAR_BOOT_CONNECT,3U*SECOND);
        }
        break;
    case RADAR_BOOT_CONNECT:
        if(!b->issued){b->issued=1U;radar_sw2_command(w,"connect");}
        if(w->state==SW2_ONLINE && w->job==SW2_JOB_NONE)
            /* Match the successful board script's40s envelope: ten legal
             *3s replies plus TX/gaps/log drainage can exceed30s. */
            enter(b,RADAR_BOOT_CHECK10,40U*SECOND);
        else if(w->state!=SW2_CONNECT_WAIT){stop(b,-710,0);return;}
        break;
    case RADAR_BOOT_CHECK10:
    case RADAR_BOOT_RESET3:
        if(!b->issued) {
            b->issued=1U;
            b->prior_count=b->stage==RADAR_BOOT_CHECK10?w->check10_passes:w->reset3_passes;
            radar_sw2_command(w,b->stage==RADAR_BOOT_CHECK10?"check10":"reset3");
        }
        if(w->job==SW2_JOB_NONE) {
            u32 count=b->stage==RADAR_BOOT_CHECK10?w->check10_passes:w->reset3_passes;
            if(w->state!=SW2_ONLINE || count!=b->prior_count+1U || w->last_error) {
                stop(b,-711,0);return;
            }
            enter(b,b->stage==RADAR_BOOT_CHECK10?RADAR_BOOT_RESET3:RADAR_BOOT_CONFIRM,30U*SECOND);
        }
        break;
    case RADAR_BOOT_CONFIRM:
        (void)radar_session_command(s,"capture confirm " RADAR_AUTOBOOT_PROFILE);
        if(!s->confirmed || strcmp(s->profile->profile_id,RADAR_AUTOBOOT_PROFILE)) {
            stop(b,-712,0);return;
        }
        enter(b,RADAR_BOOT_PREFLIGHT,SECOND);break;
    case RADAR_BOOT_PREFLIGHT:
        if(radar_session_preflight(s)){radar_session_print_preflight(s);stop(b,-713,0);return;}
        enter(b,RADAR_BOOT_HPD,30U*SECOND);break;
    case RADAR_BOOT_HPD:
        /* Raw HPD only: do not repeatedly configure DP, touch AUX, or train. */
        if(radar_display_monitor_present()) {
            if(!b->have_stable){b->have_stable=1U;b->stable_us=now;}
            if(now-b->stable_us>=500000U)enter(b,RADAR_BOOT_DISPLAY,10U*SECOND);
        } else b->have_stable=0U;
        break;
    case RADAR_BOOT_DISPLAY:
        /* Existing driver has one bounded synchronous training call. No RF
         * or acquisition owns the system yet. Never retry on failure. */
        (void)radar_display_command("display init",0);
        if(!radar_display_active() || (radar_display_faults()&7U)){stop(b,-714,0);return;}
        enter(b,RADAR_BOOT_DP_ACK,3U*SECOND);break;
    case RADAR_BOOT_DP_ACK:
        if(!radar_display_active() || (radar_display_faults()&7U)){stop(b,-715,0);return;}
        if(radar_display_ready()) {
            if(!b->have_stable) {
                b->have_stable=1U;b->stable_us=now;b->dp_baseline_valid=1U;
                b->dp_observations=radar_display_fifo_observations();
                b->dp_startup_flags=radar_display_faults();
                SAY(b,"[AUTOBOOT DP BASELINE] startup_flags=%x fifo_observations=%u retained; new observations reject startup/run.\r\n",
                    b->dp_startup_flags,b->dp_observations);
            }
            if(radar_display_fifo_observations()!=b->dp_observations){stop(b,-722,0);return;}
            if(now-b->stable_us>=500000U)enter(b,RADAR_BOOT_VIEW,SECOND);
        } else b->have_stable=0U;
        break;
    case RADAR_BOOT_VIEW:
        radar_display_request_page(RADAR_VIEW_TOP);
        if(radar_display_get_requested_page()!=RADAR_VIEW_TOP){stop(b,-716,0);return;}
        enter(b,RADAR_BOOT_LIVE,SECOND);break;
    case RADAR_BOOT_LIVE:
        if(!radar_display_ready() || (radar_display_faults()&7U) || radar_session_preflight(s)) {
            stop(b,-717,0);return;
        }
        (void)radar_stream_scan_command(b->stream,"scope live");
        if(!b->stream->running || b->stream->error){stop(b,-718,0);return;}
        enter(b,RADAR_BOOT_FIRST_FRAME,15U*SECOND);break;
    case RADAR_BOOT_FIRST_FRAME:
        if(!b->stream->running || !radar_display_active() || (radar_display_faults()&7U)) {
            stop(b,-719,0);return;
        }
        if(b->stream->capture.frames_verified && b->stream->completed && b->stream->presented) {
            enter(b,RADAR_BOOT_DONE,0U);
            SAY(b,"[AUTOBOOT PASS] Verified real frame, PL processing and DP ACK. Continuous TOP VIEW running; boot_us=%llu. Not a sensing accuracy claim.\r\n",
                (unsigned long long)(now-b->began_us));
        }
        break;
    default:stop(b,-720,0);break;
    }
}
static int readonly(const char *cmd)
{
    static const char *const allowed[]={"status","help","profile","response","progress",
        "dma status","dma trace","phy status","capture status","capture profile",
        "capture preflight","capture plan","scope status","display status","display diag",
        "scope spatial","scope scene","scope scene tracks","scope record status",
        "dsp diag status","scope telemetry"};
    unsigned i;for(i=0U;i<sizeof allowed/sizeof allowed[0];++i)
        if(!strcmp(cmd,allowed[i]))return 1;
    return 0;
}
int radar_autoboot_command(RadarAutoboot *b,const char *cmd)
{
    if(!strcmp(cmd,"autoboot status") || !strcmp(cmd,"autoboot")) {
        SAY(b,"[AUTOBOOT STATUS] stage=%s failure_stage=%s rc=%d active=%u profile=%s startup_dp=%x fifo_baseline=%u fifo_now=%u monitoring=%u; no automatic retry\r\n",
            radar_autoboot_stage_name(b->stage),radar_autoboot_stage_name(b->failed_stage),
            b->error,(unsigned)radar_autoboot_active(b),RADAR_AUTOBOOT_PROFILE,
            b->dp_startup_flags,b->dp_observations,radar_display_fifo_observations(),
            (unsigned)b->dp_baseline_valid);return 1;
    }
    if(!strcmp(cmd,"autoboot cancel")) {
        if(radar_autoboot_active(b))stop(b,-721,1);
        else SAY(b,"[AUTOBOOT] no pending startup; scope stop controls an already running stream.\r\n");
        return 1;
    }
    if(cmd[0] && b->stage==RADAR_BOOT_DONE && b->dp_baseline_valid && !readonly(cmd)) {
        /* A subsequent explicit run/display/control sequence owns its own
         * lifecycle. Never apply this boot's FIFO baseline to that new run. */
        b->dp_baseline_valid=0U;
        SAY(b,"[AUTOBOOT] completed result retained; monitoring handed to explicit manual control.\r\n");
    }
    if(cmd[0] && radar_autoboot_active(b) && !readonly(cmd)) {
        /* Preserve native emergency cancellation for explicit safe/disable
         * (including console Ctrl-C translated to safe). It must not become
         * a normal boundary stop just because startup still awaits frame1. */
        stop_mode(b,-721,1,!strcmp(cmd,"safe") || !strcmp(cmd,"disable"));
        /* A native safe/stop was already requested. Avoid a second request
         * through a different owner; other explicit commands are delegated. */
        if(!strcmp(cmd,"safe") || !strcmp(cmd,"disable") || !strcmp(cmd,"scope stop"))return 1;
    }
    return 0;
}
