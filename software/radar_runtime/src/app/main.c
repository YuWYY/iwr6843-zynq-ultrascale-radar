/* SPDX-License-Identifier: MIT
 * Cold-target entry; SW0 memory
 * writes are preceded by read-only PL identity and DMA-quiescence checks.
 * Loading an ELF may itself overwrite live RAM: this guard cannot make an
 * active-target hot download safe. Always reset/initialize the complete target.
 */
#include "radar_contract.h"
#include "radar_hw.h"
#include "radar_memory.h"
#include "radar_control.h"
#include "radar_sw2.h"
#include "radar_console.h"
#include "radar_sw3.h"
#include "radar_session.h"
#include "radar_display.h"
#include "radar_scan.h"
#include "radar_stream_scan.h"
#include "radar_keys.h"
#include "radar_dsp_diag.h"
#include "radar_autoboot.h"
#include "xil_printf.h"
#include "xil_assert.h"
#include <string.h>
#include <stdio.h>

static RadarControl control;
static RadarConsole console;
static RadarSw2 sw2;
static RadarSw3 sw3;
static RadarSession session;
static RadarScan scan;
static RadarStreamScan *const stream=(RadarStreamScan *)(UINTPTR)RADAR_DSP_STREAM_WORK;
_Static_assert(sizeof(RadarStreamScan)<=0x100000U,"stream reserved control area");
_Static_assert(sizeof(RadarStreamRecord)*RADAR_STREAM_LOG_CAPACITY<=0x600000U,"stream telemetry reservation");
_Static_assert(sizeof(RadarSpatialWeights)<=0x100000U,"spatial coefficient reservation");
static RadarDspDiag diag;
static RadarViewDiagnostics view_diagnostics;
static RadarAutoboot autoboot;

static void view_snapshot(void)
{
    static const char *const source_names[]={"UNKNOWN","STOPPED","STARTING","RUNNING","HELD RESET"};
    RadarViewDiagnostics *d=&view_diagnostics;
    const RadarCaptureResult *r=scan.consumer_result.valid && scan.consumer_result.session==scan.last_acquisition ?
        &scan.consumer_result : &session.result;
    memset(d,0,sizeof *d);
    d->sampled_us=radar_hw_now_us();
    if(scan.count && scan.published_session==scan.history[scan.count-1U].session) {
        d->freshness=scan.published_freshness;
        if(d->sampled_us>=scan.published_observed_us) {
            d->frame_age_valid=1U;
            d->frame_age_us=d->sampled_us-scan.published_observed_us;
        }
    }
    (void)snprintf(d->capture_state,sizeof d->capture_state,"%s",radar_session_state_name(session.state));
    (void)snprintf(d->source_state,sizeof d->source_state,"%s",
        (unsigned)session.source<sizeof source_names/sizeof source_names[0] ? source_names[session.source] : "UNKNOWN");
    (void)snprintf(d->profile_id,sizeof d->profile_id,"%s",session.profile->profile_id);
    d->captures_passed=session.captures_passed;d->captures_failed=session.captures_failed;
    d->capture_error=session.error;d->native_error=session.native_error;d->cleanup_error=session.cleanup_error;
    /* A manually acquired, not-yet-parsed result must not be labelled with
     * the preceding history frame's HSI/FFT diagnostics. */
    d->last_result_valid=r->valid && scan.count &&
        scan.history[scan.count-1U].valid &&
        r->session==scan.last_acquisition;
    if(d->last_result_valid){
        d->last_session=scan.published_session;d->last_bytes=RADAR_RANGE_INPUT_BYTES;d->last_crc32=scan.expected_crc;
        d->last_dma_control=r->final_dma.control;d->last_dma_status=r->final_dma.status;
    }
    if(stream->mode && scan.count) {
        d->last_result_valid=scan.history[scan.count-1U].valid;
        d->last_session=scan.published_session;d->last_bytes=RADAR_RANGE_INPUT_BYTES;d->last_crc32=scan.expected_crc;
        d->captures_passed=stream->capture.frames_verified;d->captures_failed=stream->errors;
        d->capture_error=stream->error;
    }
    d->sys_faults=session.sys_faults;d->phy_status=session.phy_status;d->phy_fault=session.phy_fault;
    d->control_timeouts=control.timeouts;d->control_protocol_faults=control.protocol_faults;
    d->tx_count=RADAR_RANGE_TX;d->rx_count=RADAR_RANGE_RX;
    d->chirps=RADAR_RANGE_CHIRPS;d->adc_samples=RADAR_RANGE_BINS;
}
static void assert_report(const char8 *file,s32 line)
{
    xil_printf("[ASSERT] %s:%d; target held for diagnosis.\r\n",file,(int)line);
}
static int display_busy(void)
{
    return radar_dsp_diag_owns(&diag) || session.owns || session.exporting || stream->record_exporting || sw3.job!=SW3_IDLE ||
           sw2.job!=SW2_JOB_NONE || sw2.state==SW2_CONNECT_WAIT ||
           radar_cli_busy(&sw2.cli);
}
static void idle(void)
{
#if defined(__aarch64__)
    __asm__ volatile("yield");
#endif
}
static void trim(char *s)
{
    size_t first=0U,last=strlen(s);
    while(s[first]==' '){++first;}
    while(last>first && s[last-1U]==' '){--last;}
    memmove(s,s+first,last-first);s[last-first]='\0';
}
static int cold_dma_guard(void)
{
    RadarDmaRegisters r;
    int rc=radar_dma_probe(&r);
    if(rc){return rc;}
    if((r.control&(XAXIDMA_CR_RUNSTOP_MASK|XAXIDMA_CR_RESET_MASK)) ||
       !(r.status&XAXIDMA_HALTED_MASK) || (r.status&XAXIDMA_ERR_ALL_MASK)) { return -1; }
    return 0;
}
static int diag_may_acquire(void)
{
    /* Diagnostic inputs never borrow a live capture, processing or display
     * lease. A separate driver preserves production frame de-duplication. */
    return !stream->running && !stream->exporting && !stream->record_exporting &&
           !radar_stream_scan_evidence_held(stream) && !stream->capture.raw.used &&
           !stream->capture.metadata_open && !radar_scan_busy(&scan) && !scan.frames.count && !scan.frames.lease &&
           !session.owns && !session.exporting && !session.copy_lease &&
           !session.raw_store_owner && !control.pending &&
           sw3.job==SW3_IDLE && sw2.job==SW2_JOB_NONE && sw2.state==SW2_SAFE &&
           !radar_cli_busy(&sw2.cli) && !radar_display_active() &&
           session.source!=RADAR_SOURCE_STARTING && session.source!=RADAR_SOURCE_RUNNING &&
           cold_dma_guard()==0;
}
int main(void)
{
    int rc;
    char command[RADAR_CONSOLE_LINE_BYTES];
    Xil_AssertSetCallback(assert_report);
    xil_printf("\r\n========== F20 RADAR RUNTIME / SW4.1 ==========\r\n");
    xil_printf("Version: %s\r\n",RADAR_SW_VERSION);
    xil_printf("Build: %s\r\n",RADAR_SW_BUILD_ID);
    xil_printf("[INFO] XSA expected: %s\r\n",RADAR_EXPECTED_XSA_SHA256);
    xil_printf("[INFO] BIT expected: %s\r\n",RADAR_EXPECTED_BIT_SHA256);
    xil_printf("[INFO] Expected identities only, not loaded-bit/flash hash. Initial state SAFE; deployment autoboot=%u, all native admission gates retained.\r\n",(unsigned)RADAR_AUTOBOOT_ENABLE);
    rc=radar_hw_check_bsp();
    if(!rc){rc=radar_control_run_sw1(&control);radar_control_print_diagnostics(&control);}
    if(!rc){rc=cold_dma_guard();}
    if(!rc){rc=radar_memory_prepare();}
    if(rc){
        u64 last=radar_hw_now_us();
        xil_printf("[FAIL] prerequisites rc=%d; runtime blocked. Save log; complete target reset/PL/PS initialization required.\r\n",rc);
        for(;;){
            if(radar_hw_now_us()-last>=UINT64_C(5000000)){
                last=radar_hw_now_us();xil_printf("[ALIVE] SW2/SW3/SW4 BLOCKED; no speculative DMA reset or memory re-use\r\n");
            }
            idle();
        }
    }
    xil_printf("[PASS] SW0/SW1 and cold DMA guard. Starting SAFE console, not a payload/PHY board PASS.\r\n");
    radar_console_init(&console);
    radar_sw2_init(&sw2,&control,&console);
    radar_sw3_init(&sw3,&sw2,&control,&console);
    radar_session_init(&session,&sw3,&sw2,&control,&console);
    radar_display_init(&console);
    radar_scan_init(&scan,&session);
    rc=radar_stream_scan_init(stream,&scan,(RadarStreamRecord *)(UINTPTR)RADAR_DSP_TELEMETRY_WORK,
        (void *)(UINTPTR)RADAR_DSP_SCENE_WORK,0x100000U,(RadarSpatialWeights *)(UINTPTR)RADAR_DSP_WEIGHTS_WORK);
    if(rc) { xil_printf("[FAIL] STREAM workspace init rc=%d\r\n",rc);for(;;)idle(); }
    radar_dsp_diag_init(&diag,&console,&scan.dsp.io);
    radar_keys_init();
    radar_autoboot_init(&autoboot,&scan,stream,&console,RADAR_AUTOBOOT_ENABLE);
    for(;;){
        if(radar_stream_active(&stream->capture)){radar_stream_poll(&stream->capture);}
        else if(session.owns){radar_session_poll(&session);}
        else{
            radar_sw3_poll(&sw3);
            radar_sw2_poll(&sw2);
            radar_session_poll(&session); /* bounded retained-result export only */
        }
        radar_dsp_diag_poll(&diag);
        if(!radar_dsp_diag_owns(&diag)) {
            if(stream->mode)radar_stream_scan_poll(stream);else radar_scan_poll(&scan);
        }
        radar_keys_poll();
        view_snapshot();
        radar_display_live_quiet(stream->mode);
        radar_display_scope_scene_update(scan.history,scan.count,scan.status,scan.revision,
            scan.active,&view_diagnostics,&scan.spatial_frame,stream->mode?&stream->scene_frame:NULL);
        /* Rendering consumes cached snapshots. In pipeline mode the native
         * UART/control phases still get priority; preallocated RX may run or
         * wait HALTED while the independent graphics buffer is painted. */
        radar_display_poll((stream->running && radar_stream_display_idle(&stream->capture)) ? 0 : scan.pipeline && scan.display_decoupled && session.owns &&
            (session.state==RADAR_SESSION_RECEIVE || session.state==RADAR_SESSION_COPY ||
             radar_session_display_idle(&session)) ? 0 : display_busy());
        radar_scan_presented(&scan,radar_display_scope_presented());
        stream->dp_fault_bits|=radar_display_faults();stream->dp_active=radar_display_active();
        if(stream->mode)radar_stream_scan_ack(stream,radar_display_last_ack());
        else radar_scan_display_ack(&scan,radar_display_last_ack());
        if(radar_console_poll(&console,command,sizeof(command))){
            trim(command);
            if(radar_autoboot_command(&autoboot,command))continue;
            if(!strcmp(command,"display off") && radar_stream_scan_evidence_held(stream) &&
               stream->recorder.state!=RADAR_RECORD_FROZEN && stream->recorder.state!=RADAR_RECORD_ARMED) {
                radar_console_printf(&console,"[RECORD BLOCK] Stop/drain and acknowledge the final frame before display off.\r\n");
                continue;
            }
            /* Frozen records own the telemetry reservation until explicit
             * record clear. Do not let another diagnostic/FINITE command
             * repurpose it through a different CLI dispatch path. */
            if(command[0] && radar_stream_scan_evidence_held(stream) &&
               strncmp(command,"scope record",12U) && strcmp(command,"scope status") &&
               strcmp(command,"scope scene") && strcmp(command,"scope scene tracks") && strncmp(command,"scope telemetry",15U) &&
               strcmp(command,"scope spatial") && strncmp(command,"scope beam",10U) &&
               strcmp(command,"scope live") && strcmp(command,"scope stop") &&
               strcmp(command,"scope dsp recover") && strcmp(command,"capture recover") &&
               strcmp(command,"capture clear") && strcmp(command,"capture status") &&
               strcmp(command,"dma status") && strcmp(command,"dma trace") &&
               strncmp(command,"display view",12U) && strcmp(command,"display status") &&
               strcmp(command,"display diag") && strcmp(command,"display off") && strcmp(command,"display init") &&
               strcmp(command,"safe") && strcmp(command,"disable") && strcmp(command,"status")) {
                radar_console_printf(&console,"[RECORD BLOCK] Evidence owns reserved memory; export and scope record clear before other work.\r\n");
                continue;
            }
            if(command[0] && radar_dsp_diag_command(&diag,command,
                !radar_dsp_diag_owns(&diag) && diag_may_acquire()))continue;
            if(radar_dsp_diag_owns(&diag)) {
                if(!strcmp(command,"safe") || !strcmp(command,"disable")) {
                    radar_dsp_diag_cancel(&diag);
                } else if(strcmp(command,"status")) {
                    radar_console_printf(&console,"[DIAG BLOCK] Diagnostic lease held; use dsp diag status/cancel/recover/release.\r\n");
                    continue;
                }
            }
            if(command[0] && radar_display_view_command(command)) { continue; }
            if ((strncmp(command,"scope start",11U)==0 || strcmp(command,"scope live")==0 ||
                 strcmp(command,"scope show")==0) &&
                !radar_display_active() && !scan.display_decoupled) {
                radar_console_printf(&console,"[SCOPE] Run display init before scope start/live/show.\r\n");
                continue;
            }
            if (command[0] && radar_stream_scan_command(stream,command)) { continue; }
            if (command[0] && radar_scan_command(&scan,command)) { continue; }
            if(session.exporting && strncmp(command,"display",7U)==0 &&
               (command[7]==' ' || command[7]=='\0')){
                radar_console_printf(&console,"[DISPLAY] Finish export before display commands.\r\n");
                continue;
            }
            if(command[0] && !radar_display_command(command,display_busy()) &&
               !radar_session_command(&session,command) &&
               !radar_sw3_command(&sw3,command)){
                radar_sw2_command(&sw2,command);
            }
        }
        radar_autoboot_poll(&autoboot);
        idle();
    }
}
