/* SPDX-License-Identifier: MIT
 * Linker-owned memory, not unchecked fixed physical addresses.
 * This is startup provisioning, NOT a general live-remap/DMA recovery routine.
 */
#include "radar_contract.h"
#include "radar_memory.h"
#include "radar_hw.h"
#include "xil_cache.h"
#include "xil_mmu.h"
#include "xil_printf.h"
#include "xaxidma.h"
#include "bspconfig.h"

extern u8 __radar_bd_zone_start[], __radar_bd_zone_end[];
extern u8 __radar_rx_pool_start[], __radar_rx_pool_end[];
extern u8 __radar_capture_store_start[], __radar_capture_store_end[];
extern u8 __radar_display_frames_start[], __radar_display_frames_end[];
extern u8 __radar_display_control_start[], __radar_display_control_end[];
extern u8 __radar_dsp_workspace_start[], __radar_dsp_workspace_end[];
extern u8 __radar_normal_end[], __radar_ddr_start[], __radar_ddr_end[];
extern u8 _heap_start[], _heap_end[], _el3_stack_end[], __el3_stack[];
extern u8 __mmu_tbl0_start[], __mmu_tbl2_end[];

UINTPTR radar_memory_bd_base(void) { return (UINTPTR)__radar_bd_zone_start; }
UINTPTR radar_memory_rx_base(void) { return (UINTPTR)__radar_rx_pool_start; }
UINTPTR radar_memory_capture_base(void) { return (UINTPTR)__radar_capture_store_start; }
UINTPTR radar_memory_display_frame(u32 index)
{
    return (index < 2U) ? (UINTPTR)__radar_display_frames_start +
                         (UINTPTR)index * RADAR_DISPLAY_SLOT_BYTES : 0U;
}
UINTPTR radar_memory_display_control(void) { return (UINTPTR)__radar_display_control_start; }
UINTPTR radar_memory_dsp_workspace(void) { return (UINTPTR)__radar_dsp_workspace_start; }
UINTPTR radar_memory_rx_slice(u32 index)
{
    if (index >= RADAR_RX_BD_COUNT) { return 0U; }
    return radar_memory_rx_base() + ((UINTPTR)index * RADAR_RX_SLICE_BYTES);
}
static int within(UINTPTR p, UINTPTR b, UINTPTR e) { return (p >= b) && (p < e); }

/* Two patterns on one cache line. Only reserved SW0-owned lines are written.
 * Final contents are zero; this is a small access smoke test, not DDR signoff. */
static int test_line(UINTPTR base, int cached)
{
    volatile u32 *p = (volatile u32 *)base;
    u32 k, pass;
    for (pass = 0U; pass < 2U; ++pass) {
        for (k = 0U; k < 16U; ++k) {
            u32 v = (u32)base ^ (k * 0x01010101U) ^ 0xA5965A69U;
            p[k] = (pass == 0U) ? v : ~v;
        }
        radar_hw_barrier();
        if (cached != 0) {
            Xil_DCacheFlushRange(base, RADAR_CACHE_LINE_BYTES);
            Xil_DCacheInvalidateRange(base, RADAR_CACHE_LINE_BYTES);
        }
        radar_hw_barrier();
        for (k = 0U; k < 16U; ++k) {
            u32 v = (u32)base ^ (k * 0x01010101U) ^ 0xA5965A69U;
            u32 expected = (pass == 0U) ? v : ~v;
            u32 got = p[k];
            if (got != expected) {
                radar_hw_print_hex64("[FAIL] Reserved memory mismatch at ", base + k * 4U);
                xil_printf("[FAIL] expected=%08x actual=%08x\r\n", expected, got);
                return -1;
            }
        }
    }
    for (k = 0U; k < 16U; ++k) { p[k] = 0U; }
    if (cached != 0) { Xil_DCacheFlushRange(base, RADAR_CACHE_LINE_BYTES); }
    radar_hw_barrier();
    return 0;
}

int radar_memory_prepare(void)
{
    const UINTPTR b = (UINTPTR)__radar_bd_zone_start;
    const UINTPTR be = (UINTPTR)__radar_bd_zone_end;
    const UINTPTR r = (UINTPTR)__radar_rx_pool_start;
    const UINTPTR re = (UINTPTR)__radar_rx_pool_end;
    const UINTPTR result = (UINTPTR)__radar_capture_store_start;
    const UINTPTR result_end = (UINTPTR)__radar_capture_store_end;
    const UINTPTR display = (UINTPTR)__radar_display_frames_start;
    const UINTPTR display_end = (UINTPTR)__radar_display_frames_end;
    const UINTPTR dc = (UINTPTR)__radar_display_control_start;
    const UINTPTR dc_end = (UINTPTR)__radar_display_control_end;
    const UINTPTR dsp=(UINTPTR)__radar_dsp_workspace_start,dsp_end=(UINTPTR)__radar_dsp_workspace_end;
    const UINTPTR normal_end = (UINTPTR)__radar_normal_end;
    const UINTPTR ddr_start = (UINTPTR)__radar_ddr_start;
    const UINTPTR ddr_end = (UINTPTR)__radar_ddr_end;
    volatile u32 stack_probe = 0U;
    u32 i;
    const u32 actual_ring_bytes =
        (u32)XAxiDma_BdRingMemCalc(XAXIDMA_BD_MINIMUM_ALIGNMENT, RADAR_RX_BD_COUNT);

    if ((be <= b) || (re <= r) || (ddr_end <= ddr_start) ||
        ((b & (RADAR_MMU_BLOCK_BYTES - 1U)) != 0U) ||
        ((be - b) != RADAR_BD_ZONE_BYTES) || ((re - r) != RADAR_RX_POOL_BYTES) ||
        ((r & (RADAR_MMU_BLOCK_BYTES - 1U)) != 0U) || (r != be) ||
        (normal_end > b) || (b < ddr_start) || (re > ddr_end) ||
        (result_end>ddr_end) || (result!=re) || (result_end<=result) || ((result_end-result)!=RADAR_CAPTURE_STORE_BYTES) ||
        (display != result_end) || (display != 0x01400000U) ||
        (display_end - display != RADAR_DISPLAY_FRAMES_BYTES) ||
        (dc != display_end) || (dc != 0x02400000U) ||
        (dc_end - dc != RADAR_DISPLAY_CONTROL_BYTES) || (dc_end > ddr_end) ||
        dsp!=dc_end || dsp!=0x02600000U || dsp_end!=0x03600000U ||
        dsp_end-dsp!=RADAR_DSP_WORKSPACE_BYTES || dsp_end>ddr_end ||
        ((u64)dsp_end > RADAR_DDR_APP_LIMIT) || (actual_ring_bytes > RADAR_BD_ZONE_BYTES) ||
        (!within((UINTPTR)&stack_probe, (UINTPTR)_el3_stack_end, (UINTPTR)__el3_stack)) ||
        (!within((UINTPTR)&radar_memory_prepare, ddr_start, normal_end)) ||
        ((UINTPTR)_heap_end > normal_end) || ((UINTPTR)__el3_stack > normal_end) ||
        ((UINTPTR)__mmu_tbl2_end > normal_end) ||
        (((UINTPTR)_heap_end - (UINTPTR)_heap_start) < 65536U) ||
        (((UINTPTR)__el3_stack - (UINTPTR)_el3_stack_end) < 16384U)) {
        xil_printf("[FAIL] Linker layout/stack mismatch. Use lscript_sw4.ld and tools/check_build.py.\r\n");
        return -1;
    }
    radar_hw_print_range("[MEM] Normal image", ddr_start, normal_end);
    radar_hw_print_range("[MEM] Heap        ", (UINTPTR)_heap_start, (UINTPTR)_heap_end);
    radar_hw_print_range("[MEM] EL3 stack   ", (UINTPTR)_el3_stack_end, (UINTPTR)__el3_stack);
    radar_hw_print_range("[MEM] SG BD zone  ", b, be);
    radar_hw_print_range("[MEM] RX pool     ", r, re);
    radar_hw_print_range("[MEM] CPU result  ", result, result_end);
    radar_hw_print_range("[MEM] DP frames WB", display, display_end);
    radar_hw_print_range("[MEM] DP ctrl NC  ", dc, dc_end);
    radar_hw_print_range("[MEM] DSP work WB ",dsp,dsp_end);
    xil_printf("[MEM] Future RX: %u slices x %u bytes; driver ring bytes=%u\r\n",
               RADAR_RX_BD_COUNT, RADAR_RX_SLICE_BYTES, actual_ring_bytes);

    /* Check identity mappings before writing reserved memory. No remap of MMIO.
     * This program is only supported with the default EL3 identity-map BSP. */
    for (i = 0U; i < RADAR_RX_POOL_BYTES / RADAR_MMU_BLOCK_BYTES; ++i) {
        if (radar_hw_check_translation(r + (UINTPTR)i * RADAR_MMU_BLOCK_BYTES, 0xFFU, 1) != 0) {
            return -1;
        }
    }
    if (radar_hw_check_translation(b, 0xFFU, 1) != 0) {
        xil_printf("[FAIL] BD zone not at default cold-start attributes; restart target cleanly.\r\n");
        return -1;
    }
    if (radar_hw_check_translation(dc, 0xFFU, 1) != 0) {
        xil_printf("[FAIL] Display control region requires a clean target restart.\r\n");
        return -1;
    }
    for (i = 0U; i < RADAR_DISPLAY_FRAMES_BYTES / RADAR_MMU_BLOCK_BYTES; ++i) {
        if (radar_hw_check_translation(display + (UINTPTR)i * RADAR_MMU_BLOCK_BYTES, 0xFFU, 1) != 0) {
            return -1;
        }
    }
    /* Region is isolated and no DMA/other A53 owns it. Clean old lines FIRST. */
    Xil_DCacheFlushRange(b, RADAR_BD_ZONE_BYTES);
    radar_hw_barrier();
    Xil_SetTlbAttributes(b, NORM_NONCACHE);
    /* DPDMA embeds descriptors in its driver instance. Its native graphics
     * path does not flush them, so it owns a separate Normal-NC MMU block. */
    Xil_DCacheFlushRange(dc, RADAR_DISPLAY_CONTROL_BYTES);
    radar_hw_barrier();
    Xil_SetTlbAttributes(dc, NORM_NONCACHE);
    radar_hw_barrier();
    if ((radar_hw_check_translation(b, 0x44U, 1) != 0) ||
        (radar_hw_check_translation(dc, 0x44U, 1) != 0) ||
        (radar_hw_check_translation(dc_end - 1U, 0x44U, 1) != 0) ||
        (radar_hw_check_translation(be - 1U, 0x44U, 1) != 0) ||
        (radar_hw_check_translation((UINTPTR)&stack_probe, 0xFFU, 1) != 0) ||
        (radar_hw_check_translation((UINTPTR)&radar_memory_prepare, 0xFFU, 0) != 0) ||
        (radar_hw_check_translation((UINTPTR)__mmu_tbl0_start, 0xFFU, 1) != 0)) {
        return -1;
    }
    for (i = 0U; i < RADAR_RX_POOL_BYTES / RADAR_MMU_BLOCK_BYTES; ++i) {
        if (radar_hw_check_translation(r + (UINTPTR)i * RADAR_MMU_BLOCK_BYTES, 0xFFU, 1) != 0) {
            return -1;
        }
    }
    xil_printf("[PASS] BD zone Normal-NC; code/stack/page-tables/RX remain Normal-WB\r\n");
    if ((test_line(b, 0) != 0) || (test_line(be - RADAR_CACHE_LINE_BYTES, 0) != 0)) {
        return -1;
    }
    for (i = 0U; i < RADAR_RX_BD_COUNT; ++i) {
        UINTPTR s = radar_memory_rx_slice(i);
        if ((test_line(s, 1) != 0) ||
            (test_line(s + RADAR_RX_SLICE_BYTES - RADAR_CACHE_LINE_BYTES, 1) != 0)) {
            return -1;
        }
    }
    xil_printf("[PASS] Reserved-line smoke check: 258 x 64B locations, two patterns\r\n");
    for (i=0U;i<RADAR_CAPTURE_STORE_BYTES/RADAR_MMU_BLOCK_BYTES;++i) {
        if (radar_hw_check_translation(result+(UINTPTR)i*RADAR_MMU_BLOCK_BYTES,0xFFU,1)!=0) { return -1; }
    }
    if (test_line(result,1)!=0 || test_line(result_end-RADAR_CACHE_LINE_BYTES,1)!=0) { return -1; }
    xil_printf("[INFO] CPU result store has 2 extra smoke-test lines; no whole-DDR scan, no submitted SG descriptor\r\n");
    /* Only reserved lines, before display initialization. Never touch the
     * display descriptors or pixels from a live capture recovery path. */
    if (test_line(dc, 0) != 0 || test_line(dc_end - RADAR_CACHE_LINE_BYTES, 0) != 0) { return -1; }
    for (i = 0U; i < 2U; ++i) {
        UINTPTR f = radar_memory_display_frame(i);
        if (test_line(f, 1) != 0 || test_line(f + RADAR_DISPLAY_SLOT_BYTES - RADAR_CACHE_LINE_BYTES, 1) != 0) { return -1; }
    }
    for(i=0;i<RADAR_DSP_WORKSPACE_BYTES/RADAR_MMU_BLOCK_BYTES;++i) {
        if(radar_hw_check_translation(dsp+(UINTPTR)i*RADAR_MMU_BLOCK_BYTES,0xFFU,1))return -1;
    }
    if(test_line(dsp,1) || test_line(dsp_end-RADAR_CACHE_LINE_BYTES,1))return -1;
    /* Initial state header is explicitly invalid. Temporal cells are initialized
     * by a new configuration epoch; no stale NOLOAD state is certified valid. */
    {
        volatile u32 *temporal=(volatile u32 *)(UINTPTR)0x02851000U;
        for(i=0;i<16U;++i)temporal[i]=0U;
        Xil_DCacheFlushRange((UINTPTR)temporal,64U);radar_hw_barrier();
    }
    xil_printf("[PASS] DP control Normal-NC; pixels and isolated16MiB DSP workspace Normal-WB\r\n");
    return 0;
}
