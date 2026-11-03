/* SPDX-License-Identifier: MIT */
#ifndef RADAR_MEMORY_H
#define RADAR_MEMORY_H
#include "xil_types.h"
/* Called only at application cold start, before any DMA/other core uses DDR. */
int radar_memory_prepare(void);
UINTPTR radar_memory_bd_base(void);
UINTPTR radar_memory_rx_base(void);
UINTPTR radar_memory_capture_base(void);
UINTPTR radar_memory_rx_slice(u32 index);
#define RADAR_DISPLAY_SLOT_BYTES 0x00800000U
#define RADAR_DISPLAY_FRAMES_BYTES (2U * RADAR_DISPLAY_SLOT_BYTES)
#define RADAR_DISPLAY_CONTROL_BYTES 0x00200000U
UINTPTR radar_memory_display_frame(u32 index);
UINTPTR radar_memory_display_control(void);
#define RADAR_DSP_WORKSPACE_BYTES 0x01000000U
UINTPTR radar_memory_dsp_workspace(void);
#endif
