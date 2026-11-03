/* SPDX-License-Identifier: MIT
 * Bounded polled XUartPs transport, strict line/prompt recognizer.
 * No console passthrough. SW4 commands come only from a validated compiled profile.
 */
#include "radar_cli.h"
#include "radar_contract.h"
#include "radar_hw.h"
#include "xuartps_hw.h"
#include "xstatus.h"
#include <string.h>

#define UART_ERRORS (XUARTPS_IXR_OVER | XUARTPS_IXR_FRAMING | \
                     XUARTPS_IXR_PARITY | XUARTPS_IXR_RBRK | XUARTPS_IXR_TOVR)
_Static_assert(RADAR_CLI_RX_BUDGET <= 64U, "Bound receive work per loop");
_Static_assert(RADAR_CLI_DRAIN_QUIET_US < RADAR_CLI_DRAIN_LIMIT_US, "Drain bound");
_Static_assert(sizeof(RADAR_CLI_QUERY_COMMAND) + sizeof(RADAR_CLI_LINE_END)
               <= RADAR_CLI_TX_CAPACITY, "Query too long");

static int lower_ascii(int c) { return (c >= 'A' && c <= 'Z') ? c + ('a'-'A') : c; }
static int contains_ci(const char *s, const char *needle)
{
    size_t i, j, n=strlen(needle), len=strlen(s);
    if (n == 0U || n > len) { return 0; }
    for (i=0U; i+n<=len; ++i) {
        for (j=0U; j<n && lower_ascii((unsigned char)s[i+j]) ==
                              lower_ascii((unsigned char)needle[j]); ++j) {}
        if (j == n) { return 1; }
    }
    return 0;
}
static void finish(RadarCli *c, int result)
{
    c->result=result; c->terminal=1U;
    c->state=(result==RADAR_CLI_OK) ? RADAR_CLI_SUCCESS : RADAR_CLI_FAILED;
    if (result==RADAR_CLI_OK) {
        ++c->passed;
        if (c->require_version) { c->version_valid=1U;c->version_epoch=c->reset_epoch; }
    }
    else {
        ++c->failed;
        /* A failed command may already have been interpreted remotely.
         * Hardware receive errors also invalidate the stream epoch. */
        if (c->tx_sent != 0U || result==RADAR_CLI_E_UART) { c->needs_reset=1U; }
    }
}
static int profile_valid(void)
{
    /* This phase intentionally authorizes ONLY the SDK read-only version
     * query. A different firmware needs an explicitly reviewed profile. */
    return strcmp(RADAR_CLI_QUERY_COMMAND,"version")==0 &&
           (strcmp(RADAR_CLI_LINE_END,"\n")==0 ||
            strcmp(RADAR_CLI_LINE_END,"\r")==0) &&
           strlen(RADAR_CLI_PAYLOAD_PREFIX)>0U &&
           strlen(RADAR_CLI_DONE_LINE)>0U && strlen(RADAR_CLI_PROMPT)>0U;
}
void radar_cli_init(RadarCli *c)
{
    memset(c,0,sizeof(*c)); c->state=RADAR_CLI_IDLE;
}
int radar_cli_prepare(RadarCli *c)
{
    XUartPs_Config *cfg;
    XUartPsFormat f, actual;
    if (c==NULL || !profile_valid()) { return RADAR_CLI_E_PROFILE; }
    if (radar_cli_busy(c)) { return RADAR_CLI_BUSY; }
    if (radar_hw_check_translation((UINTPTR)RADAR_UART1_BASE,0x00U,1)!=0) {
        return RADAR_CLI_E_BSP;
    }
    cfg=XUartPs_LookupConfig((UINTPTR)RADAR_UART1_BASE);
    if (cfg==NULL || cfg->BaseAddress!=RADAR_UART1_BASE ||
        cfg->ModemPinsConnected!=0) { return RADAR_CLI_E_BSP; }
    if (XUartPs_CfgInitialize(&c->uart,cfg,RADAR_UART1_BASE)!=XST_SUCCESS) {
        return RADAR_CLI_E_UART;
    }
    c->initialized=1U;
    XUartPs_SetInterruptMask(&c->uart,0U);
    XUartPs_SetOptions(&c->uart,0U); /* no break, no auto RTS/CTS */
    XUartPs_SetOperMode(&c->uart,XUARTPS_OPER_MODE_NORMAL);
    f.BaudRate=RADAR_CLI_BAUD; f.DataBits=XUARTPS_FORMAT_8_BITS;
    f.Parity=XUARTPS_FORMAT_NO_PARITY; f.StopBits=XUARTPS_FORMAT_1_STOP_BIT;
    if (XUartPs_SetDataFormat(&c->uart,&f)!=XST_SUCCESS) {
        radar_cli_disable(c); return RADAR_CLI_E_UART;
    }
    XUartPs_SetFifoThreshold(&c->uart,1U);
    XUartPs_SetRecvTimeout(&c->uart,0U);
    XUartPs_WriteReg(RADAR_UART1_BASE,XUARTPS_ISR_OFFSET,XUARTPS_IXR_MASK);
    XUartPs_WriteReg(RADAR_UART1_BASE,XUARTPS_CR_OFFSET,
        (XUartPs_ReadReg(RADAR_UART1_BASE,XUARTPS_CR_OFFSET) & ~XUARTPS_CR_STARTBRK) |
        XUARTPS_CR_STOPBRK);
    XUartPs_EnableUart(&c->uart);
    radar_hw_barrier();
    XUartPs_GetDataFormat(&c->uart,&actual);
    if (actual.BaudRate!=f.BaudRate || actual.DataBits!=f.DataBits ||
        actual.Parity!=f.Parity || actual.StopBits!=f.StopBits ||
        XUartPs_GetOperMode(&c->uart)!=XUARTPS_OPER_MODE_NORMAL ||
        (XUartPs_ReadReg(RADAR_UART1_BASE,XUARTPS_MODEMCR_OFFSET) &
         XUARTPS_MODEMCR_FCM)!=0U) {
        radar_cli_disable(c); return RADAR_CLI_E_UART;
    }
    c->enabled=1U; c->state=RADAR_CLI_IDLE; c->terminal=0U;
    c->uart_errors=0U; c->tx_sent=0U;
    /* Do not erase the last response or the ambiguous-transaction latch. */
    return RADAR_CLI_OK;
}
void radar_cli_disable(RadarCli *c)
{
    if (c!=NULL) {
        if (c->initialized) {
            XUartPs_SetInterruptMask(&c->uart,0U);
            XUartPs_DisableUart(&c->uart);
            radar_hw_barrier();
        }
        c->enabled=0U;
    }
}
int radar_cli_tx_idle(const RadarCli *c)
{
    u32 s;
    if (!c->initialized) { return 1; }
    s=XUartPs_ReadReg(RADAR_UART1_BASE,XUARTPS_SR_OFFSET);
    return (s & XUARTPS_SR_TXEMPTY)!=0U && (s & XUARTPS_SR_TACTIVE)==0U;
}
int radar_cli_busy(const RadarCli *c)
{
    return c->state==RADAR_CLI_DRAIN || c->state==RADAR_CLI_TX ||
           c->state==RADAR_CLI_RESPONSE;
}
static int start_command(RadarCli *c,const char *command,int require_version,u64 now)
{
    size_t n,e,i;
    if (!c || !c->initialized || !c->enabled) { return RADAR_CLI_E_NOT_READY; }
    if (radar_cli_busy(c) || c->terminal) { return RADAR_CLI_BUSY; }
    if (c->needs_reset) { return RADAR_CLI_E_RESET_REQUIRED; }
    if (!profile_valid() || !command) { return RADAR_CLI_E_PROFILE; }
    n=strlen(command);e=strlen(RADAR_CLI_LINE_END);
    if (!n || n+e>=sizeof(c->tx)) { return RADAR_CLI_E_PROFILE; }
    for (i=0;i<n;++i) {
        if ((unsigned char)command[i]<32U || (unsigned char)command[i]>126U || command[i]==';') { return RADAR_CLI_E_PROFILE; }
    }
    memcpy(c->tx,command,n);memcpy(c->tx+n,RADAR_CLI_LINE_END,e);
    c->tx[n+e]='\0';c->tx_len=(u32)(n+e);c->tx_sent=0U;
    c->rx_len=0U;c->line_len=0U;c->rx[0]='\0';c->line[0]='\0';
    c->require_version=(u8)(require_version!=0);
    c->allow_finite_stop_warning=0U;c->finite_stop_warnings=0U;
    c->require_actual_start=0U;
    if (require_version) { c->version_valid=0U;c->last_version[0]='\0'; }
    c->payload_seen=0U;c->done_seen=0U;c->prompt_seen=0U;
    c->uart_errors=0U;c->result=RADAR_CLI_BUSY;
    c->started_us=now;c->stage_us=now;c->quiet_since_us=now;c->last_rx_us=now;
    c->state=RADAR_CLI_DRAIN;++c->queries;
    return RADAR_CLI_OK;
}
int radar_cli_start_query(RadarCli *c,u64 now)
{
    return start_command(c,RADAR_CLI_QUERY_COMMAND,1,now);
}
int radar_cli_start_capture(RadarCli *c,const RadarCaptureProfile *p,
                            RadarCaptureCommand kind,u32 index,u64 now)
{
    int result;
    const char *command=radar_capture_profile_command(p,kind,index);
    if (!command) { return RADAR_CLI_E_PROFILE; }
    result=start_command(c,command,0,now);
    if (result==RADAR_CLI_OK && kind==RADAR_CAPTURE_CMD_STOP &&
        (p->qualification==RADAR_CAPTURE_FINITE_FRAME_TEST || p->qualification==RADAR_CAPTURE_STREAM_FRAME_TEST)) { c->allow_finite_stop_warning=1U; }
    if (result==RADAR_CLI_OK && (kind==RADAR_CAPTURE_CMD_START ||
        kind==RADAR_CAPTURE_CMD_START_REUSE)) { c->require_actual_start=1U; }
    return result;
}
static int signed_decimal32(const char **at)
{
    const char *s=*at; u32 value=0U,limit=2147483647U,digit;
    if (*s=='-') { limit=2147483648U; ++s; }
    if (*s<'0' || *s>'9') { return 0; }
    do {
        digit=(u32)(*s-'0');
        if (value>(limit-digit)/10U) { return 0; }
        value=value*10U+digit; ++s;
    } while (*s>='0' && *s<='9');
    *at=s; return 1;
}
static int finite_stop_warning(const char *s)
{
    static const char prefix[]="mmWave Stop error ignored [Error code: ";
    static const char middle[]=" Subsystem: ";
    if (strncmp(s,prefix,sizeof(prefix)-1U)!=0) { return 0; }
    s+=sizeof(prefix)-1U;
    if (!signed_decimal32(&s) || strncmp(s,middle,sizeof(middle)-1U)!=0) { return 0; }
    s+=sizeof(middle)-1U;
    return signed_decimal32(&s) && s[0]==']' && s[1]=='\0';
}
static void classify_line(RadarCli *c)
{
    char *s=c->line, *e;
    size_t n;
    while (*s==' ' || *s=='\t') { ++s; }
    e=s+strlen(s);
    while (e>s && (e[-1]==' ' || e[-1]=='\t')) { *--e='\0'; }
    if (*s=='\0') { return; }
    /* TI returns Done even when START was ignored. Such a reply cannot
     * establish a new acquisition or a valid configuration-reuse epoch. */
    if (c->require_actual_start && strcmp(s,"Ignored: Sensor is already started")==0) {
        finish(c,RADAR_CLI_E_REMOTE); return;
    }
    /* SDK 3.5 MMWave_stop can report a warning after a finite source stopped
     * itself. Only this exact line in an admitted finite sensorStop may pass;
     * it is NOT an ACK and still requires Done, prompt and the quiet window. */
    if (c->allow_finite_stop_warning && finite_stop_warning(s)) {
        ++c->finite_stop_warnings; return;
    }
    if (contains_ci(s,"error") || contains_ci(s,"not recognized") ||
        contains_ci(s,"exception") || contains_ci(s,"assert")) {
        finish(c,RADAR_CLI_E_REMOTE); return;
    }
    if (strcmp(s,RADAR_CLI_DONE_LINE)==0) {
        if (!c->require_version || c->payload_seen) { c->done_seen=1U; }
        return;
    }
    n=strlen(RADAR_CLI_PAYLOAD_PREFIX);
    if (strncmp(s,RADAR_CLI_PAYLOAD_PREFIX,n)==0) {
        const char *v=s+n;
        while (*v==' ' || *v=='\t') { ++v; }
        if (*v==':') {
            ++v; while (*v==' ' || *v=='\t') { ++v; }
            if (*v>='0' && *v<='9') {
                size_t len=strlen(s);
                c->payload_seen=1U;
                if (len>=sizeof(c->last_version)) { finish(c,RADAR_CLI_E_OVERFLOW);return; }
                if (c->require_version) { memcpy(c->last_version,s,len+1U); }
            }
        }
    }
}
static void accept_byte(RadarCli *c, u8 ch)
{
    if (c->rx_len+1U >= RADAR_CLI_RX_CAPACITY) {
        finish(c,RADAR_CLI_E_OVERFLOW); return;
    }
    c->rx[c->rx_len++]=(char)ch; c->rx[c->rx_len]='\0';
    if (ch=='\r' || ch=='\n') {
        classify_line(c); c->line_len=0U; c->line[0]='\0'; return;
    }
    if (ch!='\t' && (ch<32U || ch>126U)) {
        finish(c,RADAR_CLI_E_PROTOCOL); return;
    }
    /* Extra non-whitespace after a terminal prompt is not silently ignored. */
    if (c->prompt_seen && ch!=' ' && ch!='\t') {
        finish(c,RADAR_CLI_E_PROTOCOL); return;
    }
    if (c->line_len+1U >= RADAR_CLI_LINE_CAPACITY) {
        finish(c,RADAR_CLI_E_OVERFLOW); return;
    }
    c->line[c->line_len++]=(char)ch; c->line[c->line_len]='\0';
    if ((!c->require_version || c->payload_seen) && c->done_seen &&
        strcmp(c->line,RADAR_CLI_PROMPT)==0) { c->prompt_seen=1U; }
}
void radar_cli_poll(RadarCli *c, u64 now)
{
    u8 data[RADAR_CLI_RX_BUDGET];
    u32 n,i,err;
    if (!c || !c->initialized || !c->enabled) { return; }
    err=XUartPs_ReadReg(RADAR_UART1_BASE,XUARTPS_ISR_OFFSET) & UART_ERRORS;
    n=XUartPs_Recv(&c->uart,data,(u32)sizeof(data));
    err |= XUartPs_ReadReg(RADAR_UART1_BASE,XUARTPS_ISR_OFFSET) & UART_ERRORS;
    if (err!=0U) {
        c->uart_errors |= err; c->total_uart_errors |= err;
        XUartPs_WriteReg(RADAR_UART1_BASE,XUARTPS_ISR_OFFSET,err);
        if (radar_cli_busy(c)) { finish(c,RADAR_CLI_E_UART); }
        else { c->needs_reset=1U; }
    }
    if (n!=0U) { c->last_rx_us=now; c->quiet_since_us=now; }
    /* Bytes arriving before the first actual TX byte cannot belong to this
     * command. Restart the pre-send quiet window rather than accepting them. */
    if (c->state==RADAR_CLI_TX && c->tx_sent==0U && n!=0U) {
        c->state=RADAR_CLI_DRAIN;
    }
    if (c->state==RADAR_CLI_DRAIN || !radar_cli_busy(c)) {
        c->discarded_bytes+=n;
    } else {
        for (i=0U; i<n && radar_cli_busy(c); ++i) { accept_byte(c,data[i]); }
    }
    if (!radar_cli_busy(c)) { return; }
    /* A peer can keep producing isolated stale bytes exactly at each quiet
     * boundary. An overall deadline prevents repeated pre-TX drain restarts
     * from extending a command forever. */
    if ((now-c->started_us)>=(u64)RADAR_CLI_DRAIN_LIMIT_US + RADAR_CLI_TIMEOUT_US) {
        finish(c,c->tx_sent ? RADAR_CLI_E_TIMEOUT : RADAR_CLI_E_NO_QUIET); return;
    }
    if (c->state==RADAR_CLI_DRAIN) {
        if ((now-c->stage_us)>=RADAR_CLI_DRAIN_LIMIT_US) {
            finish(c,RADAR_CLI_E_NO_QUIET); return;
        }
        if ((now-c->quiet_since_us)>=RADAR_CLI_DRAIN_QUIET_US && radar_cli_tx_idle(c)) {
            c->state=RADAR_CLI_TX; c->stage_us=now;
        }
        return;
    }
    if ((now-c->stage_us)>=RADAR_CLI_TIMEOUT_US) {
        finish(c,RADAR_CLI_E_TIMEOUT); return;
    }
    if (c->state==RADAR_CLI_TX) {
        if ((now-c->stage_us)>=RADAR_CLI_TX_TIMEOUT_US) {
            finish(c,RADAR_CLI_E_TX_TIMEOUT); return;
        }
        if (c->tx_sent<c->tx_len) {
            n=XUartPs_Send(&c->uart,(u8*)c->tx+c->tx_sent,c->tx_len-c->tx_sent);
            c->tx_sent+=n;
        }
        if (c->tx_sent==c->tx_len && radar_cli_tx_idle(c)) {
            c->state=RADAR_CLI_RESPONSE;
        }
    }
    if (c->state==RADAR_CLI_RESPONSE && (!c->require_version || c->payload_seen) && c->done_seen &&
        c->prompt_seen && radar_cli_tx_idle(c) &&
        (now-c->last_rx_us)>=RADAR_CLI_END_QUIET_US) {
        finish(c,RADAR_CLI_OK);
    }
}
void radar_cli_cancel(RadarCli *c)
{
    if (radar_cli_busy(c)) { finish(c,RADAR_CLI_E_CANCELED); }
}
void radar_cli_new_reset_epoch(RadarCli *c)
{
    c->needs_reset=0U; c->uart_errors=0U; c->terminal=0U;
    c->allow_finite_stop_warning=0U;c->finite_stop_warnings=0U;
    c->require_actual_start=0U;
    c->version_valid=0U; c->last_version[0]='\0'; ++c->reset_epoch;
    c->state=RADAR_CLI_IDLE; c->tx_sent=0U;
    if (c->initialized) {
        XUartPs_WriteReg(RADAR_UART1_BASE,XUARTPS_ISR_OFFSET,XUARTPS_IXR_MASK);
    }
}
int radar_cli_take_result(RadarCli *c, int *result)
{
    if (!c || !result || !c->terminal) { return 0; }
    *result=c->result; c->terminal=0U; c->state=RADAR_CLI_IDLE; return 1;
}
const char *radar_cli_result_name(int r)
{
    switch (r) {
    case RADAR_CLI_OK:return "PASS";
    case RADAR_CLI_BUSY:return "BUSY";
    case RADAR_CLI_E_NOT_READY:return "NOT_CONNECTED";
    case RADAR_CLI_E_PROFILE:return "PROFILE_INVALID";
    case RADAR_CLI_E_BSP:return "UART1_BSP_OR_MAPPING";
    case RADAR_CLI_E_UART:return "UART_HARDWARE_ERROR";
    case RADAR_CLI_E_TX_TIMEOUT:return "TX_TIMEOUT";
    case RADAR_CLI_E_TIMEOUT:return "RESPONSE_TIMEOUT";
    case RADAR_CLI_E_OVERFLOW:return "RESPONSE_OVERFLOW";
    case RADAR_CLI_E_REMOTE:return "REMOTE_ERROR";
    case RADAR_CLI_E_PROTOCOL:return "RESPONSE_FORMAT";
    case RADAR_CLI_E_CANCELED:return "CANCELED";
    case RADAR_CLI_E_NO_QUIET:return "RX_NOT_QUIET";
    case RADAR_CLI_E_RESET_REQUIRED:return "REMOTE_RESET_REQUIRED";
    default:return "UNKNOWN";
    }
}
