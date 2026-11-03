/* SPDX-License-Identifier: MIT */
#include "radar_contract.h"
#include "radar_hw.h"
#include "bspconfig.h"
#include "xparameters.h"
#include "xaxidma.h"
#include "xgpio.h"
#include "xuartps.h"
#include "xil_printf.h"

#if !defined(__aarch64__) || !defined(SDT)
#error "SW0 requires Vitis 2025.1 A53-0 64-bit standalone (SDT), not R5/M1 SDK."
#endif
#if !defined(PLATFORM_ZYNQMP) || (XPAR_CPU_ID != 0)
#error "Select ZynqMP psu_cortexa53_0."
#endif
#if !defined(EL3) || (EL3 != 1)
#error "SW0 targets the default standalone EL3 BSP; do not select EL1 non-secure."
#endif
#if !defined(STDOUT_BASEADDRESS) || (STDOUT_BASEADDRESS != 0xFF000000)
#error "BSP stdout must be PS UART0 (0xFF000000), not radar UART1."
#endif
#if !defined(STDIN_BASEADDRESS) || (STDIN_BASEADDRESS != 0xFF000000)
#error "BSP stdin must be PS UART0 (0xFF000000)."
#endif
_Static_assert(sizeof(UINTPTR) == 8U, "AArch64 address type required");
_Static_assert(RADAR_RX_SLICE_BYTES * RADAR_RX_BD_COUNT == RADAR_RX_POOL_BYTES,
               "RX pool geometry mismatch");

u32 radar_hw_current_el(void)
{
    u64 v;
    __asm__ volatile ("mrs %0, CurrentEL" : "=r" (v));
    return (u32)((v >> 2U) & 3U);
}
u64 radar_hw_sctlr(void)
{
    u64 v;
    __asm__ volatile ("mrs %0, sctlr_el3" : "=r" (v));
    return v;
}
u64 radar_hw_counter(void)
{
    u64 v;
    __asm__ volatile ("isb\n\tmrs %0, cntpct_el0" : "=r" (v) :: "memory");
    return v;
}
u32 radar_hw_counter_hz(void)
{
    u64 v;
    __asm__ volatile ("mrs %0, cntfrq_el0" : "=r" (v));
    return (u32)v;
}
u64 radar_hw_now_us(void)
{
    const u64 ticks = radar_hw_counter();
    const u32 hz = radar_hw_counter_hz();
    if (hz == 0U) { return 0U; }
    /* Avoid ticks*1000000 overflow; caller uses elapsed differences. */
    return (ticks / hz) * UINT64_C(1000000)
           + ((ticks % hz) * UINT64_C(1000000)) / hz;
}
void radar_hw_barrier(void)
{
    __asm__ volatile ("dsb sy\n\tisb" ::: "memory");
}
void radar_hw_print_hex64(const char *label, u64 v)
{
    xil_printf("%s0x%08x%08x\r\n", label, (u32)(v >> 32U), (u32)v);
}
void radar_hw_print_range(const char *label, UINTPTR b, UINTPTR e)
{
    xil_printf("%s [0x%08x%08x, 0x%08x%08x) bytes=%u\r\n", label,
               (u32)((u64)b >> 32U), (u32)b, (u32)((u64)e >> 32U), (u32)e,
               (u32)(e - b));
}

int radar_hw_check_translation(UINTPTR address, u8 expected_attr, int writable)
{
    u64 par;
    if (writable != 0) {
        __asm__ volatile ("at s1e3w, %0\n\tisb" :: "r" (address) : "memory");
    } else {
        __asm__ volatile ("at s1e3r, %0\n\tisb" :: "r" (address) : "memory");
    }
    __asm__ volatile ("mrs %0, par_el1" : "=r" (par));
    if (((par & 1U) != 0U) || ((u8)(par >> 56U) != expected_attr) ||
        ((par & UINT64_C(0x0000FFFFFFFFF000)) !=
         ((u64)address & UINT64_C(0x0000FFFFFFFFF000)))) {
        radar_hw_print_hex64("[FAIL] Translation address=", address);
        radar_hw_print_hex64("[FAIL] PAR_EL1=", par);
        xil_printf("[FAIL] Expected identity mapping, attr=0x%02x, writable=%u\r\n",
                   (u32)expected_attr, (u32)(writable != 0));
        return -1;
    }
    return 0;
}

int radar_hw_check_bsp(void)
{
    XAxiDma_Config *d;
    XGpio_Config *g;
    XUartPs_Config *u0;
    XUartPs_Config *u1;
    u64 before, after;
    volatile u32 spin;
    if (radar_hw_current_el() != 3U) {
        xil_printf("[FAIL] CurrentEL is not EL3. Use default A53 standalone BSP.\r\n");
        return -1;
    }
    if ((radar_hw_sctlr() & 5U) != 5U) {
        xil_printf("[FAIL] MMU/D-cache disabled. Use normal BSP startup, not DDR diag startup.\r\n");
        return -1;
    }
    if (radar_hw_counter_hz() == 0U) {
        xil_printf("[FAIL] A53 counter frequency is zero.\r\n");
        return -1;
    }
    before = radar_hw_counter();
    for (spin = 0U; spin < 1000U; ++spin) { __asm__ volatile ("nop"); }
    after = radar_hw_counter();
    if (after <= before) {
        xil_printf("[FAIL] A53 physical counter did not advance.\r\n");
        return -1;
    }
    xil_printf("[PASS] CPU=A53-0, EL3, 64-bit; MMU/D-cache enabled\r\n");
    xil_printf("[PASS] Physical counter=%u Hz (not CPU or PL frequency)\r\n",
               radar_hw_counter_hz());

    /* Lookup ONLY: reads generated configuration tables in application memory.
     * Do NOT replace with CfgInitialize: DMA init may reset actual hardware. */
    d = XAxiDma_LookupConfig((UINTPTR)RADAR_DMA_BASE);
    g = XGpio_LookupConfig((UINTPTR)RADAR_GPIO_BASE);
    u0 = XUartPs_LookupConfig(RADAR_UART0_BASE);
    u1 = XUartPs_LookupConfig(RADAR_UART1_BASE);
    if ((d == NULL) || (g == NULL) || (u0 == NULL) || (u1 == NULL)) {
        xil_printf("[FAIL] M2 peripheral absent from BSP tables. Rebuild M2 Platform.\r\n");
        return -1;
    }
    if ((d->BaseAddr != RADAR_DMA_BASE) || (d->HasSg != 1) ||
        (d->HasS2Mm != 1) || (d->HasMm2S != 0) || (d->S2MmDataWidth != 64) ||
        (d->S2MmBurstSize != 64) || (d->SgLengthWidth != 23) ||
        (d->AddrWidth != 32) || (d->MicroDmaMode != 0) ||
        (d->HasS2MmDRE != 0) || (d->HasStsCntrlStrm != 0) ||
        (d->S2MmNumChannels != 1)) {
        xil_printf("[FAIL] BSP DMA sg=%u rx=%u tx=%u width=%u lenbits=%u addr=%u\r\n",
                   (u32)d->HasSg, (u32)d->HasS2Mm, (u32)d->HasMm2S,
                   (u32)d->S2MmDataWidth, (u32)d->SgLengthWidth, (u32)d->AddrWidth);
        return -1;
    }
    if ((g->BaseAddress != RADAR_GPIO_BASE) || (g->IsDual != 1) ||
        (g->Width != 32U) || (g->InterruptPresent != 0)) {
        xil_printf("[FAIL] BSP GPIO must be the unique dual-32-bit M2 GPIO.\r\n");
        return -1;
    }
    if ((u0->BaseAddress != RADAR_UART0_BASE) ||
        (u1->BaseAddress != RADAR_UART1_BASE) || (u1->ModemPinsConnected != 0)) {
        xil_printf("[FAIL] UART BSP mapping/modem mismatch.\r\n");
        return -1;
    }
    xil_printf("[PASS] BSP table: DMA A0000000, SG=1 S2MM=1 MM2S=0 width=64 len=23\r\n");
    xil_printf("[PASS] BSP table: GPIO A0010000 dual=1 width=32 irq=0\r\n");
    xil_printf("[PASS] BSP table: UART0 FF000000, UART1 FF010000\r\n");
    xil_printf("[PASS] BSP stdin/stdout=PS UART0\r\n");
    xil_printf("[INFO] Expected PL ABI=0x%08x; NOT read at SW0\r\n", RADAR_EXPECTED_ABI);
    return 0;
}
