/* SPDX-License-Identifier: MIT */
#ifndef RADAR_CONSOLE_H
#define RADAR_CONSOLE_H
#include "xil_types.h"
#include <stddef.h>
#define RADAR_CONSOLE_LINE_BYTES 80U
#define RADAR_CONSOLE_TX_BYTES   8192U

typedef struct {
    char tx[RADAR_CONSOLE_TX_BYTES];
    u32 head,tail,count,dropped,overflow_announced;
    char line[RADAR_CONSOLE_LINE_BYTES];
    u32 line_len,rx_errors;
    u8 discarding,skip_lf;
} RadarConsole;
void radar_console_init(RadarConsole *c); /* does not reconfigure UART0 */
/* Drains a bounded TX budget and accepts at most one whole input line. */
int radar_console_poll(RadarConsole *c, char *out, size_t capacity);
int radar_console_printf(RadarConsole *c,const char *fmt,...)
#if defined(__GNUC__)
    __attribute__((format(printf,2,3)))
#endif
    ;
void radar_console_response(RadarConsole *c,const char *data,size_t n);
u32 radar_console_free(const RadarConsole *c);
#endif
