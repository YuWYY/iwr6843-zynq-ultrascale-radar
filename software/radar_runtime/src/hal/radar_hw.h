/* SPDX-License-Identifier: MIT */
#ifndef RADAR_HW_H
#define RADAR_HW_H
#include "xil_types.h"
/* These routines never access PL MMIO or initialize/reset DMA/GPIO/UART1. */
int radar_hw_check_bsp(void);
u32 radar_hw_current_el(void);
u64 radar_hw_sctlr(void);
u64 radar_hw_counter(void);
u32 radar_hw_counter_hz(void);
u64 radar_hw_now_us(void);
void radar_hw_barrier(void);
void radar_hw_print_hex64(const char *label, u64 value);
void radar_hw_print_range(const char *label, UINTPTR begin, UINTPTR end);
/* EL3 / 4KiB-granule default A53 BSP. Check a read/write translation without
 * accessing the target memory. expected_attr: 0x44=Normal NC, 0xFF=Normal WB. */
int radar_hw_check_translation(UINTPTR address, u8 expected_attr, int writable);
#endif
