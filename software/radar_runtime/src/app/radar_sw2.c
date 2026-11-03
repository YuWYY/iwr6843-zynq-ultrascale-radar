/* SPDX-License-Identifier: MIT
 * SW2 = user-controlled board IO + read-only TI CLI query. One owner of the
 * existing mailbox. NO CFG/ARM/RECAL/RECOVER/DMA transaction is issued here.
 * UART/query/boot waits are non-blocking. GPIO handshakes use the existing
 * SHORT bounded poll interface (1ms); an AXI transaction itself must return.
 */
#include "radar_sw2.h"
#include "radar_hw.h"
#include <string.h>

#define IO_ACTIVE        (UINT32_C(1)<<0U)
#define IO_UART          UINT32_C(3) /* IO_ALLOW + UART_ALLOW, SYNC_LEVEL=0 */
#define IO_RESET         UINT32_C(5) /* IO_ALLOW + RESET_HOLD, UART_ALLOW=0 */
#define PG_MASK          (RADAR_FLAG_PG|RADAR_FLAG_PG_QUALIFIED)
#define CAPTURE_ACTIVE   (RADAR_FLAG_SESSION_OPEN|RADAR_FLAG_CMD_PENDING| \
                          RADAR_FLAG_SNAPSHOT_PENDING|RADAR_FLAG_RECOVERING| \
                          RADAR_FLAG_ARM_PENDING|RADAR_FLAG_PHY_BUSY)
#define SAY(s,...) ((void)radar_console_printf((s)->console,__VA_ARGS__))

const char *radar_sw2_state_name(RadarSw2State st)
{
    switch(st) {
    case SW2_SAFE:return "SAFE"; case SW2_CONNECT_WAIT:return "CONNECT_WAIT";
    case SW2_ONLINE:return "ONLINE"; case SW2_QUERY:return "QUERY";
    case SW2_REPEAT_WAIT:return "REPEAT_WAIT"; case SW2_RESET_HOLD:return "RESET_HOLD";
    case SW2_RESET_BOOT:return "RESET_BOOT"; case SW2_FAULT:return "FAULT";
    case SW2_CAPTURE:return "SW4_OWNED";
    default:return "UNKNOWN";
    }
}
static int read_io(RadarSw2 *s)
{
    int rc=radar_control_read32(s->control,RADAR_PAGE_FLAGS,&s->flags);
    if (rc==0) { rc=radar_control_read32(s->control,RADAR_PAGE_IO_REQUEST,&s->io_read); }
    s->last_gpio_rc=(u32)rc;
    return rc;
}
static int pg_ok(const RadarSw2 *s)
{
    return (s->flags & PG_MASK)==PG_MASK && (s->flags & RADAR_FLAG_POWER_LOST)==0U;
}
static int no_capture(const RadarSw2 *s) { return (s->flags & CAPTURE_ACTIVE)==0U; }
static int outputs_match(const RadarSw2 *s,u32 io)
{
    u32 expected;
    if (s->io_read!=io) { return 0; }
    expected=(io==IO_UART) ? RADAR_FLAG_UART_OE|RADAR_FLAG_SYNC_OE :
             (io==IO_RESET ? RADAR_FLAG_NRST_LOW : 0U);
    return (s->flags & RADAR_SW1_OUTPUT_MASK)==expected &&
           (io==0U || ((s->flags & IO_ACTIVE)!=0U && pg_ok(s)));
}
static int write_io(RadarSw2 *s,u16 value)
{
    RadarControlReply r;
    int rc;
    u64 start;
    u32 polls;
    s->io_verified=0U;
    rc=radar_control_command_begin(s->control,RADAR_OP_IO_SET,0U,value,RADAR_CTRL_ACK_TIMEOUT_US);
    if (rc==0) {
        do { rc=radar_control_command_poll(s->control,&r); } while (rc==RADAR_CONTROL_PENDING);
    }
    s->last_gpio_rc=(u32)rc;
    if (rc!=0) { return rc; }
    if (r.value!=value) { return RADAR_CONTROL_E_PROTOCOL; }
    start=radar_hw_now_us();
    for (polls=0U;polls<RADAR_CTRL_MAX_POLLS;++polls) {
        rc=read_io(s); if (rc!=0) { return rc; }
        if (outputs_match(s,value)) {
            s->expected_io=value; s->io_verified=1U; return 0;
        }
        if (radar_hw_now_us()-start>=RADAR_IO_CONFIRM_TIMEOUT_US) { break; }
    }
    return RADAR_CONTROL_E_VERIFY;
}
static void close_io(RadarSw2 *s,const char *reason,int fault)
{
    int rc;
    if (fault) {
        SAY(s,"[FAULT OBSERVED] %s; last_sampled_flags=%08x io=%08x uart_errors=%08x\r\n",
            reason,(unsigned)s->flags,(unsigned)s->io_read,(unsigned)s->cli.uart_errors);
    }
    radar_cli_cancel(&s->cli);
    radar_cli_disable(&s->cli); /* emergency cancel can truncate a character */
    s->cli.terminal=0U; s->cli.state=RADAR_CLI_IDLE;
    s->job=SW2_JOB_NONE; s->cli_verified=0U;
    rc=write_io(s,0U);
    s->state=(fault || rc!=0) ? SW2_FAULT : SW2_SAFE;
    if (rc!=0) {
        s->fatal_control=1U;
        SAY(s,"[FAULT] %s; IO_OFF NOT CONFIRMED, GPIO=%s. No retry or auto reset; stop/reinitialize board.\r\n",
            reason,radar_control_status_name(rc));
        SAY(s,"[MAILBOX] last_write=%08x last_observed=%08x desynced=%u; do not guess a new toggle\r\n",
            (unsigned)s->control->last_request,(unsigned)s->control->last_observed,
            (unsigned)s->control->desynced);
    } else {
        if (!fault && s->check10_passes && s->reset3_passes) { ++s->verified_safe_returns; }
        SAY(s,"[%s] %s; IO_SET=0 verified: TX/SYNC high-Z, NRST released; remote_reset_required=%u\r\n",
            fault?"FAULT":"SAFE",reason,(unsigned)s->cli.needs_reset);
    }
}
static int precheck(RadarSw2 *s,int require_nerr)
{
    int rc;
    if (!s->wiring_confirmed) {
        SAY(s,"[REFUSE] First confirm wiring/power/mux and non-autostart firmware, then type: confirm io\r\n"); return 0;
    }
    if (s->fatal_control || s->control->desynced) {
        SAY(s,"[REFUSE] Mailbox ownership uncertain; restart with current bit, not a blind toggle retry\r\n"); return 0;
    }
    rc=read_io(s);
    if (rc!=0) { close_io(s,"cannot read current control state",1); return 0; }
    if (!no_capture(s)) {
        SAY(s,"[REFUSE] Existing acquisition/PHY operation active; SW2 will not reset it\r\n"); return 0;
    }
    if ((s->flags & PG_MASK)!=PG_MASK) {
        SAY(s,"[REFUSE] 3V3-present/qualification missing: flags=%08x. No IO enable issued\r\n",(unsigned)s->flags); return 0;
    }
    if (require_nerr && (s->flags & RADAR_FLAG_NERR_N)==0U) {
        SAY(s,"[REFUSE] NERR is low. Inspect hardware; explicit reset is available after power confirmation\r\n"); return 0;
    }
    return 1;
}
static int start_query(RadarSw2 *s)
{
    int rc=radar_cli_start_query(&s->cli,radar_hw_now_us());
    if (rc!=0) {
        SAY(s,"[REFUSE] query: %s; reset is required after ambiguous/partial previous commands\r\n",radar_cli_result_name(rc));
        s->job=SW2_JOB_NONE; s->state=SW2_ONLINE; return 0;
    }
    s->state=SW2_QUERY;
    SAY(s,"[RUN] version query %u/%u; one command, bounded response, no sensorStart\r\n",
        (unsigned)(s->job_done+1U),(unsigned)s->job_total);
    return 1;
}
static void begin_reset(RadarSw2 *s)
{
    int rc;
    if (!precheck(s,0)) { s->job=SW2_JOB_NONE; return; }
    if (radar_cli_busy(&s->cli) || (s->cli.enabled && !radar_cli_tx_idle(&s->cli))) {
        SAY(s,"[REFUSE] UART transaction active. Use safe to cancel first\r\n"); s->job=SW2_JOB_NONE; return;
    }
    radar_cli_disable(&s->cli);
    rc=write_io(s,0U);
    if (rc==0) { rc=write_io(s,(u16)IO_RESET); }
    if (rc!=0) { close_io(s,"NRST assertion not confirmed",1); return; }
    s->cli_verified=0U; s->state=SW2_RESET_HOLD; s->stage_us=radar_hw_now_us();
    SAY(s,"[RESET] %u/%u NRST pull-low request verified, nominal hold=%u us (not pin-voltage measurement)\r\n",
        (unsigned)(s->job_done+1U),(unsigned)s->job_total,(unsigned)RADAR_RESET_HOLD_US);
}
static void print_status(RadarSw2 *s)
{
    u32 sys_faults=0U,phy_faults=0U;
    int rc=read_io(s);
    if (rc==0) { rc=radar_control_read32(s->control,RADAR_PAGE_SYS_FAULTS,&sys_faults); }
    if (rc==0) { rc=radar_control_read32(s->control,RADAR_PAGE_PHY_FAULT,&phy_faults); }
    SAY(s,"[SW2] state=%s confirm=%u IO_verified=%u CLI_verified=%u profile=%s\r\n",
        radar_sw2_state_name(s->state),(unsigned)s->wiring_confirmed,(unsigned)s->io_verified,
        (unsigned)s->cli_verified,RADAR_PROFILE_NAME);
    if (rc!=0) { SAY(s,"[GPIO] %s\r\n",radar_control_status_name(rc)); return; }
    SAY(s,"[PL] flags=%08x io=%08x pg=%u qualified=%u nerr_n=%u tx_oe=%u nrst_low=%u sync_oe=%u\r\n",
        (unsigned)s->flags,(unsigned)s->io_read,(unsigned)((s->flags>>1)&1U),
        (unsigned)((s->flags>>2)&1U),(unsigned)((s->flags>>3)&1U),
        (unsigned)((s->flags>>21)&1U),(unsigned)((s->flags>>22)&1U),(unsigned)((s->flags>>23)&1U));
    SAY(s,"[PL] sys_faults=%08x phy_faults=%08x (not cleared); RX clocks not required in SW2\r\n",
        (unsigned)sys_faults,(unsigned)phy_faults);
    SAY(s,"[MAILBOX] submitted=%u acked=%u remote_rejects=%u timeouts=%u desynced=%u\r\n",
        (unsigned)s->control->submitted,(unsigned)s->control->acknowledged,
        (unsigned)s->control->remote_rejects,(unsigned)s->control->timeouts,
        (unsigned)s->control->desynced);
    SAY(s,"[UART1] baud=%u queries=%u pass=%u fail=%u last=%s err_bits=%08x discarded=%u reset_required=%u\r\n",
        (unsigned)RADAR_CLI_BAUD,(unsigned)s->cli.queries,(unsigned)s->cli.passed,(unsigned)s->cli.failed,
        s->cli.queries==0U?"NOT_RUN":radar_cli_result_name(s->cli.result),(unsigned)s->cli.total_uart_errors,
        (unsigned)s->cli.discarded_bytes,(unsigned)s->cli.needs_reset);
}
void radar_sw2_init(RadarSw2 *s,RadarControl *control,RadarConsole *console)
{
    memset(s,0,sizeof(*s)); s->control=control; s->console=console;
    radar_cli_init(&s->cli); s->state=SW2_SAFE; s->io_verified=1U;
    s->last_monitor_us=radar_hw_now_us(); s->last_alive_us=s->last_monitor_us;
    SAY(s,"\r\n========== SW2 READY / CONTROL ONLY ==========\r\n");
    SAY(s,"[SAFE] No IO enabling or UART1 initialization at boot. No DMA/ARM/config/sensorStart.\r\n");
    SAY(s,"[PROFILE] %s; firmware=%s; capture approval is separate (capture profile)\r\n",RADAR_PROFILE_NAME,RADAR_PROFILE_FIRMWARE_ID);
    SAY(s,"[CONSOLE] ASCII at PS UART0 115200 8N1; terminate each command with CR or LF. Type help\r\n");
}
void radar_sw2_command(RadarSw2 *s,const char *line)
{
    char cmd[RADAR_CONSOLE_LINE_BYTES];
    size_t n;
    int rc;
    if (s->state==SW2_CAPTURE) { SAY(s,"[BUSY] SW4 owns UART/GPIO; use capture status or safe\r\n");return; }
    while (*line==' ' || *line=='\t') { ++line; }
    n=strlen(line); while (n && (line[n-1]==' ' || line[n-1]=='\t')) { --n; }
    if (!n) { return; }
    if (n>=sizeof(cmd)) { SAY(s,"[REJECT] Command too long\r\n"); return; }
    memcpy(cmd,line,n); cmd[n]='\0';
    if (strcmp(cmd,"safe")==0 || strcmp(cmd,"disable")==0) {
        close_io(s,"user requested safe (not radar sensorStop)",0); return;
    }
    if (strcmp(cmd,"help")==0) {
        SAY(s,"Commands: status | profile | confirm io | connect | query | check10 | reset | reset3 | response | safe\r\n");
        SAY(s,"confirm io records YOUR electrical/mux/firmware check, not a software measurement.\r\n");
        SAY(s,"reset/reset3 explicitly authorizes NRST, boot wait and version query. safe/Ctrl-C cancels; may truncate a CLI line.\r\n");
        SAY(s,"No raw send/config/capture command. query needs SDK version payload + Done + prompt. No echo-only PASS.\r\n");
        return;
    }
    if (strcmp(cmd,"status")==0) { print_status(s); return; }
    if (strcmp(cmd,"profile")==0) {
        SAY(s,"[PROFILE] query='%s' payload='%s' done='%s' prompt='%s' baud=%u\r\n",
            RADAR_CLI_QUERY_COMMAND,RADAR_CLI_PAYLOAD_PREFIX,RADAR_CLI_DONE_LINE,RADAR_CLI_PROMPT,(unsigned)RADAR_CLI_BAUD);
        SAY(s,"[PROFILE] %s; reset_hold=%u us boot_wait=%u us; adjust header only to actual firmware\r\n",
            RADAR_PROFILE_FIRMWARE_ID,(unsigned)RADAR_RESET_HOLD_US,(unsigned)RADAR_RESET_BOOT_US); return;
    }
    if (strcmp(cmd,"response")==0) {
        if (radar_cli_busy(&s->cli)) { SAY(s,"[BUSY] Response is still being received\r\n"); }
        else { radar_console_response(s->console,s->cli.rx,s->cli.rx_len); }
        return;
    }
    if (s->state!=SW2_SAFE && s->state!=SW2_ONLINE && s->state!=SW2_FAULT) {
        SAY(s,"[BUSY] %s; only status/help/response/safe available\r\n",radar_sw2_state_name(s->state)); return;
    }
    if (strcmp(cmd,"confirm io")==0) {
        s->wiring_confirmed=1U;
        SAY(s,"[USER CONFIRMED] Bank65=1.8V for present termination, Bank44=3.3V, common ground, pin/mux/NRST correct; no autostart firmware. RAM only.\r\n");
        return;
    }
    if (strcmp(cmd,"connect")==0) {
        if (s->state!=SW2_SAFE) { SAY(s,"[REFUSE] Use safe first; connect never clears remote_reset_required\r\n"); return; }
        if (!precheck(s,1)) { return; }
        rc=write_io(s,0U);
        if (rc==0) { rc=radar_cli_prepare(&s->cli); }
        if (rc==0) { rc=write_io(s,(u16)IO_UART); }
        if (rc!=0) { close_io(s,"connect setup or IO enable failed",1); return; }
        s->state=SW2_CONNECT_WAIT; s->stage_us=radar_hw_now_us();
        SAY(s,"[IO] request=3 verified: UART enabled, NRST released, SYNC driven LOW. Settling; no query sent yet\r\n"); return;
    }
    if (strcmp(cmd,"reset")==0 || strcmp(cmd,"reset3")==0) {
        s->job=(strcmp(cmd,"reset3")==0)?SW2_JOB_RESET3:SW2_JOB_RESET;
        s->job_done=0U; s->job_total=(s->job==SW2_JOB_RESET3)?3U:1U;
        begin_reset(s); return;
    }
    if (strcmp(cmd,"query")==0 || strcmp(cmd,"check10")==0) {
        if (s->state!=SW2_ONLINE) { SAY(s,"[REFUSE] Connect first\r\n"); return; }
        if (!precheck(s,1)) { return; }
        s->job=(strcmp(cmd,"check10")==0)?SW2_JOB_CHECK10:SW2_JOB_QUERY;
        s->job_done=0U; s->job_total=(s->job==SW2_JOB_CHECK10)?10U:1U;
        (void)start_query(s); return;
    }
    SAY(s,"[REJECT] Unknown/local-only command. No text forwarded to radar; type help\r\n");
}
void radar_sw2_poll(RadarSw2 *s)
{
    u64 now=radar_hw_now_us();
    int rc,result;
    if (s->state==SW2_CAPTURE) { return; } /* session owns UART and safety policy */
    radar_cli_poll(&s->cli,now); /* always service receive before console logs */
    if (!s->fatal_control && s->expected_io!=0U && now-s->last_monitor_us>=RADAR_SW2_MONITOR_US) {
        s->last_monitor_us=now;
        rc=read_io(s);
        if (rc!=0 || !pg_ok(s) || !no_capture(s) || !outputs_match(s,s->expected_io)) {
            close_io(s,"power/IO/session changed during SW2",1); return;
        }
        if (s->state!=SW2_RESET_HOLD && s->state!=SW2_RESET_BOOT &&
            (s->flags & RADAR_FLAG_NERR_N)==0U) {
            close_io(s,"NERR asserted outside reset boot window",1); return;
        }
    }
    if ((s->state==SW2_ONLINE || s->state==SW2_CONNECT_WAIT || s->state==SW2_REPEAT_WAIT) &&
        s->cli.uart_errors!=0U && !radar_cli_busy(&s->cli)) {
        close_io(s,"UART receive error outside a query; explicit reset required",1); return;
    }
    if (s->state==SW2_CONNECT_WAIT && now-s->stage_us>=RADAR_SW2_CONNECT_SETTLE_US) {
        s->state=SW2_ONLINE;
        SAY(s,"[PASS] IO enabling/readback only. UART reply NOT tested; type query. reset_required=%u\r\n",(unsigned)s->cli.needs_reset);
    } else if (s->state==SW2_RESET_HOLD && now-s->stage_us>=RADAR_RESET_HOLD_US) {
        rc=write_io(s,0U); /* release NRST, keep TX high-Z while programming UART */
        if (rc==0) { rc=radar_cli_prepare(&s->cli); }
        if (rc==0) { rc=write_io(s,(u16)IO_UART); }
        if (rc!=0) { close_io(s,"reset release/UART re-enable not confirmed",1); return; }
        s->state=SW2_RESET_BOOT; s->stage_us=radar_hw_now_us();
        SAY(s,"[RESET] released; collecting/discarding boot banner for %u us (no command yet)\r\n",(unsigned)RADAR_RESET_BOOT_US);
    } else if (s->state==SW2_RESET_BOOT && now-s->stage_us>=RADAR_RESET_BOOT_US) {
        rc=read_io(s);
        if (rc!=0 || !pg_ok(s) || (s->flags & RADAR_FLAG_NERR_N)==0U) {
            close_io(s,"radar power/NERR not ready after boot wait",1); return;
        }
        radar_cli_new_reset_epoch(&s->cli);
        (void)start_query(s);
    } else if (s->state==SW2_QUERY && radar_cli_take_result(&s->cli,&result)) {
        s->last_error=(u32)result;
        SAY(s,"[QUERY] %s tx=%u rx=%u uart_error_bits=%08x\r\n",radar_cli_result_name(result),
            (unsigned)s->cli.tx_sent,(unsigned)s->cli.rx_len,(unsigned)s->cli.uart_errors);
        if (s->job==SW2_JOB_QUERY || s->job_done==0U || result!=0) {
            radar_console_response(s->console,s->cli.rx,s->cli.rx_len);
        }
        if (result!=RADAR_CLI_OK) {
            close_io(s,"query failed; no automatic retry or reset",1); return;
        }
        s->cli_verified=1U; ++s->job_done;
        SAY(s,"[PASS] %s %u/%u: valid SDK version payload + Done + prompt\r\n",
            (s->job==SW2_JOB_CHECK10)?"check10":((s->job==SW2_JOB_QUERY)?"query":"reset/query"),
            (unsigned)s->job_done,(unsigned)s->job_total);
        if (s->job_done>=s->job_total) {
            if (s->job==SW2_JOB_CHECK10) { ++s->check10_passes; }
            if (s->job==SW2_JOB_RESET3) { ++s->reset3_passes; }
            SAY(s,"RESULT: PASS - SW2 %s ONLY; no LVDS/SG/ADC validation\r\n",
                (s->job==SW2_JOB_CHECK10)?"10 CLI QUERIES":((s->job==SW2_JOB_RESET3)?"3 RESET/CLI CYCLES":"CLI QUERY"));
            s->job=SW2_JOB_NONE; s->state=SW2_ONLINE;
        } else {
            s->state=SW2_REPEAT_WAIT; s->stage_us=now;
        }
    } else if (s->state==SW2_REPEAT_WAIT && now-s->stage_us>=RADAR_SW2_REPEAT_GAP_US &&
               radar_console_free(s->console)>6144U) {
        if (s->job==SW2_JOB_RESET3) { begin_reset(s); }
        else { (void)start_query(s); }
    }
    if (now-s->last_alive_us>=RADAR_SW2_HEARTBEAT_US &&
        (s->state==SW2_SAFE || s->state==SW2_ONLINE || s->state==SW2_FAULT)) {
        s->last_alive_us=now;
        SAY(s,"[ALIVE] SW2 state=%s CLI_verified=%u; no capture; type help\r\n",
            radar_sw2_state_name(s->state),(unsigned)s->cli_verified);
    }
}

int radar_sw2_capture_check(RadarSw2 *s)
{
    if (!s || s->state!=SW2_ONLINE || s->job!=SW2_JOB_NONE ||
        !s->wiring_confirmed || !s->io_verified || !s->cli_verified || s->fatal_control ||
        s->control->desynced || s->control->pending || !s->cli.enabled ||
        s->cli.needs_reset || s->cli.terminal || radar_cli_busy(&s->cli) ||
        !radar_cli_tx_idle(&s->cli) || s->expected_io!=IO_UART) { return -1; }
    if (read_io(s)!=0 || !pg_ok(s) || !no_capture(s) ||
        !outputs_match(s,IO_UART) || !(s->flags&RADAR_FLAG_NERR_N)) { return -1; }
    return 0;
}
int radar_sw2_capture_claim(RadarSw2 *s)
{
    if (radar_sw2_capture_check(s)!=0) { return -1; }
    s->state=SW2_CAPTURE;
    return 0;
}
int radar_sw2_capture_io(RadarSw2 *s,u16 request)
{
    int rc;
    if (!s || s->state!=SW2_CAPTURE || (request!=0U && request!=IO_RESET)) { return -1; }
    /* Only shutdown or an explicitly authorized reset hold is exposed to
     * SW4. Ordinary CLI enabling stays in the tested SW2 connect path. */
    radar_cli_cancel(&s->cli);radar_cli_disable(&s->cli);
    s->cli.terminal=0U;s->cli.state=RADAR_CLI_IDLE;s->cli_verified=0U;
    if (s->control->desynced || s->control->pending) {
        s->fatal_control=1U;s->io_verified=0U;return RADAR_CONTROL_E_DESYNC;
    }
    rc=write_io(s,request);
    if (rc) { s->fatal_control=1U; }
    return rc;
}
int radar_sw2_capture_release(RadarSw2 *s,int fault)
{
    if (!s || s->state!=SW2_CAPTURE) { return -1; }
    /* Handoff is itself validated. On failure retain SW4 ownership, so a
     * failed final readback can never become a PASS + ordinary SW2 polling. */
    if (fault || s->fatal_control || s->expected_io!=0U || !s->io_verified ||
        s->cli.enabled || radar_cli_busy(&s->cli) || read_io(s)!=0 ||
        !outputs_match(s,0U) || !no_capture(s) || !pg_ok(s) ||
        !(s->flags&RADAR_FLAG_NERR_N)) {
        s->fatal_control=1U;return -1;
    }
    s->state=SW2_SAFE;s->job=SW2_JOB_NONE;s->cli_verified=0U;
    s->last_monitor_us=radar_hw_now_us();s->last_alive_us=s->last_monitor_us;
    return 0;
}
