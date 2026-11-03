/* SPDX-License-Identifier: MIT
 * Single-owner UART0 console. No unbounded receive, no output during UART1 ISR
 * (there are no UART interrupts). Queue overflow and input truncation visible.
 */
#include "radar_console.h"
#include "radar_contract.h"
#include "xuartps_hw.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

u32 radar_console_free(const RadarConsole *c) { return RADAR_CONSOLE_TX_BYTES-c->count; }
static int enqueue(RadarConsole *c,const char *s,size_t n)
{
    size_t i;
    if (n>radar_console_free(c)) { ++c->dropped; return -1; }
    for (i=0U;i<n;++i) {
        c->tx[c->head]=s[i]; c->head=(c->head+1U)%RADAR_CONSOLE_TX_BYTES;
    }
    c->count+=(u32)n; return 0;
}
int radar_console_printf(RadarConsole *c,const char *fmt,...)
{
    char b[512];
    int n;
    va_list args;
    va_start(args,fmt); n=vsnprintf(b,sizeof(b),fmt,args); va_end(args);
    if (n<0 || (size_t)n>=sizeof(b)) { ++c->dropped; return -1; }
    return enqueue(c,b,(size_t)n);
}
void radar_console_response(RadarConsole *c,const char *s,size_t n)
{
    size_t i,limit=n<1536U?n:1536U;
    static const char hex[]="0123456789ABCDEF";
    (void)radar_console_printf(c,"[RX] %u bytes (response, not an ADC frame):\r\n",(unsigned)n);
    for (i=0U;i<limit;++i) {
        unsigned char b=(unsigned char)s[i];
        if ((b>=32U && b<=126U) || b=='\r' || b=='\n' || b=='\t') {
            char ch=(char)b; (void)enqueue(c,&ch,1U);
        } else {
            char escaped[4]={'\\','x',hex[b>>4U],hex[b&15U]};
            (void)enqueue(c,escaped,4U);
        }
    }
    (void)radar_console_printf(c,"\r\n[RX END]%s\r\n",
                              n>limit?" display limited to 1536B; full RX kept in RAM":"");
}
void radar_console_init(RadarConsole *c)
{
    unsigned i;
    memset(c,0,sizeof(*c));
    /* Do not interpret serial bytes buffered before this application started. */
    for (i=0U;i<128U && XUartPs_IsReceiveData(RADAR_UART0_BASE);++i) {
        (void)XUartPs_ReadReg(RADAR_UART0_BASE,XUARTPS_FIFO_OFFSET);
    }
}
int radar_console_poll(RadarConsole *c,char *out,size_t capacity)
{
    unsigned i;
    u32 err;
    if (!c || !out || capacity<2U) { return 0; }
    if (c->dropped!=c->overflow_announced && radar_console_free(c)>256U) {
        c->overflow_announced=c->dropped;
        (void)radar_console_printf(c,"[WARN] UART0 log queue: %u messages dropped; reduce command rate\r\n",
                                  (unsigned)c->dropped);
    }
    for (i=0U;i<32U && c->count!=0U && !XUartPs_IsTransmitFull(RADAR_UART0_BASE);++i) {
        XUartPs_WriteReg(RADAR_UART0_BASE,XUARTPS_FIFO_OFFSET,(u8)c->tx[c->tail]);
        c->tail=(c->tail+1U)%RADAR_CONSOLE_TX_BYTES; --c->count;
    }
    err=XUartPs_ReadReg(RADAR_UART0_BASE,XUARTPS_ISR_OFFSET) &
        (XUARTPS_IXR_OVER|XUARTPS_IXR_PARITY|XUARTPS_IXR_FRAMING|XUARTPS_IXR_RBRK);
    if (err) {
        c->rx_errors |= err; c->discarding=1U;
        XUartPs_WriteReg(RADAR_UART0_BASE,XUARTPS_ISR_OFFSET,err);
    }
    for (i=0U;i<32U && XUartPs_IsReceiveData(RADAR_UART0_BASE);++i) {
        u8 b=(u8)XUartPs_ReadReg(RADAR_UART0_BASE,XUARTPS_FIFO_OFFSET);
        if (b==3U) { /* Ctrl-C is an explicit emergency safe command. */
            c->line_len=0U; c->discarding=0U; c->skip_lf=0U;
            if (capacity>=5U) { memcpy(out,"safe",5U); return 1; }
            return 0;
        }
        if (b=='\n' && c->skip_lf) { c->skip_lf=0U; continue; }
        c->skip_lf=0U;
        if (b=='\r' || b=='\n') {
            c->skip_lf=(u8)(b=='\r');
            if (c->discarding || c->line_len>=capacity) {
                (void)radar_console_printf(c,"[REJECT] Entire console line discarded (overflow/error); nothing executed\r\n");
                c->line_len=0U; c->discarding=0U; continue;
            }
            if (c->line_len!=0U) {
                memcpy(out,c->line,c->line_len); out[c->line_len]='\0';
                c->line_len=0U; return 1;
            }
            continue;
        }
        if (c->discarding) { continue; }
        if (b==8U || b==127U) { if (c->line_len) { --c->line_len; } continue; }
        if (b<32U || b>126U) { c->discarding=1U; continue; }
        if (c->line_len+1U>=sizeof(c->line)) { c->discarding=1U; continue; }
        c->line[c->line_len++]=(char)b;
    }
    return 0;
}
