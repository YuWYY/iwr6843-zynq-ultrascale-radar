/* SPDX-License-Identifier: MIT
 * Native 2025.1 dppsu/dpdma/avbuf graphics bringup. Hardware is opt-in.
 * Adaptation of the public xdpdma_video_example initialization sequence.
 * ISR work is limited to flags, counters and a prepared graphics descriptor.
 * Scope images are rendered only into the acknowledged non-front page.
 * No submitted or active buffer can be overwritten.
 */
#include "radar_display.h"
#include "radar_view_cache.h"
#include "radar_dsp_types.h"
#include "radar_display_edid.h"
#include "radar_scope_view.h"
#include "radar_view_schedule.h"
#include "radar_range_quality.h"
#include "radar_memory.h"
#include "radar_hw.h"
#include "xdppsu.h"
#include "xdpdma.h"
#include "xavbuf.h"
#include "xavbuf_clk.h"
#include "xinterrupt_wrap.h"
#include "xparameters.h"
#include "xil_cache.h"
#include "xil_printf.h"
#include "sleep.h"
#include <string.h>
#include <stdio.h>
static RadarConsole *display_console;
#define DISPLAY_LOG(...) radar_console_printf(display_console,__VA_ARGS__)
static u8 saved_edid[512];
static unsigned saved_edid_bytes;
static RadarViewCache view_cache;
static int live_quiet;
void radar_display_live_quiet(int enabled) { live_quiet=enabled!=0; }
_Static_assert(sizeof(RadarViewSnapshot)<=RADAR_DSP_VIEW_SLOT_BYTES,"Immutable view exceeds reserved workspace slot");

#define WIDTH 1920U
#define HEIGHT 1080U
#define LINE_BYTES (WIDTH * 4U)
#define STRIDE ((LINE_BYTES + 255U) & ~255U)
#define FRAME_BYTES (LINE_BYTES * HEIGHT)
#define STORAGE_BYTES (STRIDE * HEIGHT)
_Static_assert(STORAGE_BYTES <= RADAR_DISPLAY_SLOT_BYTES, "Display page exceeds its reserved slot");
#define STANDARD_HTOTAL 2200U
#define STANDARD_VTOTAL 1125U
#define DMA_ERRORS (XDPDMA_ISR_AXI_RD_4K_CROSS_MASK | XDPDMA_ISR_WR_DATA_FIFO_FULL_MASK | \
                    XDPDMA_ISR_WR_CMD_FIFO_FULL_MASK | XDPDMA_ISR_DSCR_ERR3_MASK | \
                    XDPDMA_ISR_DATA_AXI_ERR3_MASK)
#define BLACK 0xff15100cU
#define WHITE 0xfff4f3eeU

/* Alignment is required by the native driver's embedded descriptors. */
static XDpDma dma __attribute__((section(".radar_display_control"), aligned(256)));
static XDpPsu dp;
static XAVBuf av;
static XDpDma_FrameBuffer frames[2];
static struct {
    int configured, dp_irq, dma_irq, active, fault, painted;
    int front, pending;
    u16 sequence;
    volatile u16 pending_id;
    volatile unsigned request, submitted, vsync, hpd;
    volatile u32 isr_errors, eisr_errors;
    volatile u32 dp_irq_latched,tx_fifo_overflow;
    volatile u32 tx_fifo_observations;
    u64 pending_since;
    const char *reason;
    unsigned edid_blocks;
    u32 pixel_hz;
    const RadarRangeFrame *scope_history;
    const RadarSpatialFrame *scope_spatial;
    u64 scope_render_us;
    unsigned scope_count;
    unsigned scope_submitted_count;
    const char *scope_status;
    RadarViewSchedule view;
    RadarTimingAck pending_timing,last_ack;
    u32 ack_sequence;
    RadarViewDiagnostics diagnostics;
    int scope_enabled, scope_scan_active;
} s;

/* Generated psu_init.c addresses: all PLL configuration words plus radar
 * DDR/interconnect/PL/UART consumers are read back unchanged around DP init.
 * The FSBL sets the native 1080p clock tree. Display code writes no PLL
 * or clock-divider register. Video, STC and audio controls are protected too. */
static const UINTPTR protected_clocks[] = {
    0xfd1a0020U,0xfd1a0024U,0xfd1a0028U, /* APLL */
    0xfd1a002cU,0xfd1a0030U,0xfd1a0034U, /* DPLL */
    0xfd1a0038U,0xfd1a003cU,0xfd1a0040U, /* VPLL */
    0xff5e0020U,0xff5e0024U,0xff5e0028U, /* IOPLL */
    0xff5e0030U,0xff5e0034U,0xff5e0038U, /* RPLL */
    0xfd1a0060U,0xfd1a0080U,0xfd1a00b8U,0xfd1a00bcU,
    0xfd1a00c0U,0xfd1a00c4U,0xff5e00c0U,0xff5e0074U,0xff5e0078U,
    0xfd1a0070U,0xfd1a0074U,0xfd1a007cU,0xfd1a0050U
};
static u32 protected_before[sizeof(protected_clocks)/sizeof(protected_clocks[0])];

/* Original compact 5x7 glyphs; columns are bitmaps, bit 0 is the top row. */
static const u8 digits[10][5] = {
 {62,81,73,69,62},{0,66,127,64,0},{98,81,73,73,70},{34,65,73,73,54},
 {24,20,18,127,16},{47,73,73,73,49},{62,73,73,73,50},{1,113,9,5,3},
 {54,73,73,73,54},{38,73,73,73,62}
};
static const u8 letters[26][5] = {
 {126,9,9,9,126},{127,73,73,73,54},{62,65,65,65,34},{127,65,65,34,28},
 {127,73,73,73,65},{127,9,9,9,1},{62,65,73,73,122},{127,8,8,8,127},
 {0,65,127,65,0},{32,64,65,63,1},{127,8,20,34,65},{127,64,64,64,64},
 {127,2,12,2,127},{127,4,8,16,127},{62,65,65,65,62},{127,9,9,9,6},
 {62,65,81,33,94},{127,9,25,41,70},{38,73,73,73,50},{1,1,127,1,1},
 {63,64,64,64,63},{31,32,64,32,31},{63,64,56,64,63},{99,20,8,20,99},
 {3,4,120,4,3},{97,81,73,69,67}
};
static void text(u32 *p, unsigned x, unsigned y, unsigned scale, const char *str)
{
    while (*str != '\0') {
        u8 cols[5] = {0,0,0,0,0};
        unsigned a, b, dx, dy;
        char c = *str++;
        if (c >= 'A' && c <= 'Z') { memcpy(cols, letters[c-'A'], 5); }
        else if (c >= '0' && c <= '9') { memcpy(cols, digits[c-'0'], 5); }
        else if (c == '-') { memset(cols, 8, 5); }
        else if (c == '.') { cols[2] = 64; }
        else if (c == ':') { cols[2] = 36; }
        else if (c == '/') { cols[0]=64; cols[1]=32; cols[2]=16; cols[3]=8; cols[4]=4; }
        for (a=0; a<5U; ++a) { for (b=0; b<7U; ++b) {
            if ((cols[a] & (1U<<b)) == 0U) { continue; }
            for (dy=0; dy<scale; ++dy) { for (dx=0; dx<scale; ++dx) {
                unsigned px=x+a*scale+dx, py=y+b*scale+dy;
                if (px<WIDTH && py<HEIGHT) { p[py*(STRIDE/4U)+px]=WHITE; }
            }}
        }}
        x += 6U*scale;
    }
}
static void paint_test_page(unsigned k)
{
    static const u32 bars[8] = {0xffffffffU,0xff00ffffU,0xffffff00U,0xff00ff00U,
                                0xffff00ffU,0xff0000ffU,0xffff0000U,0xff000000U};
    unsigned x, y;
    {
        u32 *p=(u32 *)radar_memory_display_frame(k);
        for (y=0; y<HEIGHT; ++y) { for (x=0; x<WIDTH; ++x) {
            p[y*(STRIDE/4U)+x]=(y>=300U && y<800U) ? bars[((x/(WIDTH/8U))+k)&7U] : BLACK;
        }}
        text(p,100,76,7,"RADAR LAB / DISPLAY TEST");
        text(p,100,174,4,"F20  AMD ZYNQ ULTRASCALE PLUS");
        text(p,100,866,5,k==0U ? "PAGE 1  1920 X 1080" : "PAGE 2  1920 X 1080");
        text(p,100,970,4,"1080P 60HZ - STATIC TEST - NO RADAR DATA");
        Xil_DCacheFlushRange((UINTPTR)p,STORAGE_BYTES);
        frames[k].Address=(UINTPTR)p;
        frames[k].Size=FRAME_BYTES;
        frames[k].Stride=STRIDE;
        frames[k].LineSize=LINE_BYTES;
    }
    radar_hw_barrier();
}
static void paint_once(void)
{
    if (s.painted) { return; }
    paint_test_page(0U);
    paint_test_page(1U);
    s.painted=1;
}
static void hpd_event(void *ref)
{
    (void)ref;
    ++s.hpd;
}
static void dp_irq(void *ref)
{
    u32 overflow=XDpPsu_ReadReg(dp.Config.BaseAddr,XDPPSU_TX_USER_FIFO_OVERFLOW);
    s.dp_irq_latched|=XDpPsu_ReadReg(dp.Config.BaseAddr,XDPPSU_INTR_STATUS);
    s.tx_fifo_overflow|=overflow;
    if (overflow!=0U) { ++s.tx_fifo_observations; }
    XDpPsu_HpdInterruptHandler(ref);
}
static void dma_irq(void *ref)
{
    u32 irq=XDpDma_ReadReg(dma.Config.BaseAddr,XDPDMA_ISR);
    u32 err=XDpDma_ReadReg(dma.Config.BaseAddr,XDPDMA_EISR);
    (void)ref;
    s.isr_errors |= irq & DMA_ERRORS;
    s.eisr_errors |= err;
    if ((irq & XDPDMA_ISR_VSYNC_INT_MASK) != 0U) {
        ++s.vsync;
        if (s.request != 0U && s.isr_errors == 0U && s.eisr_errors == 0U) {
            /* Native graphics VSYNC path, using its public operations.
             * Tag the prepared NC descriptor before triggering hardware so
             * poll can acknowledge the actual hardware descriptor ID.
             * TriggerStatus alone is NOT a DMA completion indication. */
            int first=(dma.Gfx.Channel.Current==NULL);
            XDpDma_SetupChannel(&dma,GraphicsChan);
            dma.Gfx.Channel.Current->DSCR_ID=s.pending_id;
            radar_hw_barrier();
            if (first) {
                (void)XDpDma_SetChannelState(&dma,GraphicsChan,XDPDMA_ENABLE);
                (void)XDpDma_Trigger(&dma,GraphicsChan);
            } else { (void)XDpDma_ReTrigger(&dma,GraphicsChan); }
            s.submitted=s.vsync;
            s.request=0U;
        }
        XDpDma_WriteReg(dma.Config.BaseAddr,XDPDMA_ISR,XDPDMA_ISR_VSYNC_INT_MASK);
    }
}
static int stop_graphics(void)
{
    int status;
    if (!s.configured) { return 0; }
    XDisableIntrId(dma.Config.IntrId,dma.Config.IntrParent);
    s.request=0U;
    status=XDpDma_SetChannelState(&dma,GraphicsChan,XDPDMA_DISABLE);
    XDpPsu_EnableMainLink(&dp,0);
    XAVBuf_EnableGraphicsBuffers(&av,0);
    s.active=0;
    s.pending=-1;
    s.front=-1;
    radar_view_schedule_forget_presentation(&s.view);
    if (status!=XST_SUCCESS) {
        s.fault=1;
        s.reason="DPDMA_STOP_TIMEOUT_COLD_RESTART_REQUIRED";
        return -1;
    }
    dma.Gfx.Channel.Current=NULL;
    dma.Gfx.TriggerStatus=XDPDMA_TRIGGER_DONE;
    s.painted=0;
    return 0;
}
static int configure(void)
{
    XDpPsu_Config *pc;
    XDpDma_Config *dc;
    if (s.configured) { return 0; }
    pc=XDpPsu_LookupConfig(XPAR_XDPPSU_0_BASEADDR);
    dc=XDpDma_LookupConfig(XPAR_XDPDMA_0_BASEADDR);
    if (pc==NULL || dc==NULL) { s.reason="BSP_DP_CONFIG_MISSING"; return -1; }
    memset(&dma,0,sizeof dma);
    XDpPsu_CfgInitialize(&dp,pc,pc->BaseAddr);
    XAVBuf_CfgInitialize(&av,dp.Config.BaseAddr);
    XDpDma_CfgInitialize(&dma,dc);
    if (XDpPsu_InitializeTx(&dp)!=XST_SUCCESS ||
        XDpDma_SetGraphicsFormat(&dma,RGBA8888)!=XST_SUCCESS ||
        XAVBuf_SetInputNonLiveGraphicsFormat(&av,RGBA8888)!=XST_SUCCESS ||
        XAVBuf_SetOutputVideoFormat(&av,RGB_8BPC)!=XST_SUCCESS) {
        s.reason="DP_GRAPHICS_INIT_FAILED"; return -1;
    }
    XDpDma_SetQOS(&dma,11);
    XAVBuf_InputVideoSelect(&av,XAVBUF_VIDSTREAM1_NONE,XAVBUF_VIDSTREAM2_NONLIVE_GFX);
    XAVBuf_ConfigureGraphicsPipeline(&av);
    XAVBuf_ConfigureOutputVideo(&av);
    XAVBuf_SetBlenderAlpha(&av,0,0);
    XDpPsu_CfgMsaEnSynchClkMode(&dp,0);
    XAVBuf_SetAudioVideoClkSrc(&av,XAVBUF_PS_CLK,XAVBUF_PS_CLK);
    XAVBuf_SoftReset(&av);
    XDpPsu_WriteReg(dp.Config.BaseAddr,XDPPSU_INTR_DIS,0xffffffffU);
    XDpDma_WriteReg(dma.Config.BaseAddr,XDPDMA_IDS,0xffffffffU);
    XDpDma_WriteReg(dma.Config.BaseAddr,XDPDMA_EIDS,0xffffffffU);
    XDpPsu_SetHpdEventHandler(&dp,hpd_event,&s);
    XDpPsu_SetHpdPulseHandler(&dp,hpd_event,&s);
    /* SDT wrapper shares the BSP GIC instance; do not reinitialize our own GIC. */
    if (!s.dp_irq) {
        if (XSetupInterruptSystem(&dp,(void *)dp_irq,
            dp.Config.IntrId,dp.Config.IntrParent,XINTERRUPT_DEFAULT_PRIORITY)!=XST_SUCCESS) {
            s.reason="DP_INTERRUPT_SETUP_FAILED"; return -1;
        }
        s.dp_irq=1;
    }
    if (!s.dma_irq) {
        if (XSetupInterruptSystem(&dma,(void *)dma_irq,dma.Config.IntrId,
            dma.Config.IntrParent,XINTERRUPT_DEFAULT_PRIORITY)!=XST_SUCCESS) {
            s.reason="DPDMA_INTERRUPT_SETUP_FAILED"; return -1;
        }
        s.dma_irq=1;
    }
    XDpPsu_WriteReg(dp.Config.BaseAddr,XDPPSU_INTR_EN,
                   XDPPSU_INTR_HPD_IRQ_MASK|XDPPSU_INTR_HPD_EVENT_MASK);
    XDpDma_InterruptEnable(&dma,XDPDMA_IEN_VSYNC_INT_MASK);
    s.configured=1;
    return 0;
}
static int edid_supported(void)
{
    u8 edid[512];
    unsigned i, n;
    int verdict;
    memset(edid,0,sizeof edid);
    if (XDpPsu_GetEdidBlock(&dp,edid,0)!=XST_SUCCESS) {
        s.reason="EDID_READ_FAILED"; return -1;
    }
    n=(unsigned)edid[126]+1U;
    if (n>4U) { n=4U; }
    for (i=1U;i<n;++i) {
        if (XDpPsu_GetEdidBlock(&dp,edid+i*128U,(u8)i)!=XST_SUCCESS) {
            s.reason="EDID_EXTENSION_READ_FAILED"; return -1;
        }
    }
    s.edid_blocks=n;
    saved_edid_bytes=n*128U;
    memcpy(saved_edid,edid,saved_edid_bytes);
    verdict=radar_edid_1080p60(edid,n*128U);
    if (verdict!=1) {
        s.reason=(verdict<0) ? "EDID_INVALID" : "EDID_1080P60_NOT_ADVERTISED";
        return -1;
    }
    DISPLAY_LOG("[DISPLAY] EDID standard 1920x1080p60 supported.\r\n");
    return 0;
}
static void queue_frame(unsigned index)
{
    /* Main loop is the only producer; one pending descriptor at a time. */
    XDisableIntrId(dma.Config.IntrId,dma.Config.IntrParent);
    s.pending=(int)index;
    ++s.sequence;
    if (s.sequence==0U) { ++s.sequence; }
    s.pending_id=s.sequence;
    s.submitted=0U;
    s.pending_since=radar_hw_now_us();
    XDpDma_DisplayGfxFrameBuffer(&dma,&frames[index]);
    radar_hw_barrier();
    s.request=1U;
    XEnableIntrId(dma.Config.IntrId,dma.Config.IntrParent);
}
static int pixel_clock_snapshot(void)
{
    u32 source=Xil_In32(0xfd1a0070U);
    u32 ctrl=Xil_In32(0xfd1a0038U), frac=Xil_In32(0xfd1a0040U);
    u32 pll_status=Xil_In32(0xfd1a0044U), ddr=Xil_In32(0xfd1a0080U);
    u32 mask=XAVBUF_VPLL_CTRL_PRE_SRC_MASK|XAVBUF_PLL_CTRL_FBDIV_MASK|
             XAVBUF_PLL_CTRL_DIV2_MASK|XAVBUF_PLL_CTRL_BYPASS_MASK|XAVBUF_PLL_CTRL_RESET_MASK;
    unsigned i;
    /* Native Vivado 2025.1 solver: VPLL = PSS * (89 + 6612/65536) / 2;
     * video /10, STC /55. DPLL/DDR unchanged, TOPSW moved to DPLL/3.
     * These readbacks enforce the new FSBL; never retune a PLL at runtime. */
    if ((source&0x013f3f07U)!=0x01010a00U ||
        (ctrl&mask)!=0x00015900U || (frac&0x8000ffffU)!=0x800019d4U ||
        (pll_status&XAVBUF_PLL_STATUS_VPLL_LOCK_MASK)==0U ||
        (Xil_In32(0xfd1a002cU)&mask)!=0x00014800U ||
        (Xil_In32(0xfd1a0034U)&XAVBUF_PLL_FRAC_CFG_ENABLED_MASK)!=0U ||
        (pll_status&XAVBUF_PLL_STATUS_DPLL_LOCK_MASK)==0U ||
        (ddr&0x00003f07U)!=0x200U ||
        (Xil_In32(0xfd1a00c0U)&0x01003f07U)!=0x01000303U ||
        (Xil_In32(0xfd1a007cU)&0x013f3f07U)!=0x01013700U ||
        (Xil_In32(0xfd1a0074U)&0x013f3f07U)!=0x01013002U ||
        XPAR_PSU_PSS_REF_CLK_FREQ_HZ!=33333000U) {
        s.reason="NATIVE_1080_CLOCK_BASELINE_MISMATCH";
        DISPLAY_LOG("[DISPLAY] clock baseline mismatch VIDEO=%08x VPLL=%08x FRAC=%08x LOCK=%08x DDR=%08x\r\n",
                   source,ctrl,frac,pll_status,ddr);
        return -1;
    }
    s.pixel_hz=(u32)(((u64)XPAR_PSU_PSS_REF_CLK_FREQ_HZ*(89U*65536U+6612U))/(2U*10U*65536U));
    for (i=0U;i<sizeof(protected_clocks)/sizeof(protected_clocks[0]);++i) {
        protected_before[i]=Xil_In32(protected_clocks[i]);
    }
    DISPLAY_LOG("[DISPLAY] native VPLL ctrl=%08x frac=%08x DDR=%08x; nominal pixel=%u Hz, Htotal=2200 Vtotal=1125.\r\n",
               ctrl,frac,ddr,s.pixel_hz);
    return 0;
}
static int protected_clocks_unchanged(void)
{
    unsigned i;
    for (i=0U;i<sizeof(protected_clocks)/sizeof(protected_clocks[0]);++i) {
        u32 value=Xil_In32(protected_clocks[i]);
        if (value!=protected_before[i]) {
            s.reason="PROTECTED_CLOCK_CHANGED";
            s.fault=1;
            DISPLAY_LOG("[DISPLAY] protected clock changed addr=%08x before=%08x after=%08x.\r\n",
                       (u32)protected_clocks[i],protected_before[i],value);
            return -1;
        }
    }
    return 0;
}
static int start_display(void)
{
    u8 power=1U;
    if (s.fault) { return -1; }
    if (pixel_clock_snapshot()!=0) { return -1; }
    if (configure()!=0) { return -1; }
    if (stop_graphics()!=0) { return -1; }
    s.hpd=0U;
    if (!XDpPsu_IsConnected(&dp)) { s.reason="NO_MONITOR_HPD_LOW"; return -1; }
    if (XDpPsu_AuxWrite(&dp,XDPPSU_DPCD_SET_POWER_DP_PWR_VOLTAGE,1,&power)!=XST_SUCCESS ||
        XDpPsu_AuxWrite(&dp,XDPPSU_DPCD_SET_POWER_DP_PWR_VOLTAGE,1,&power)!=XST_SUCCESS ||
        XDpPsu_GetRxCapabilities(&dp)!=XST_SUCCESS) {
        s.reason="AUX_CAPABILITY_FAILED"; return -1;
    }
    if (dp.LinkConfig.MaxLaneCount<2U || dp.LinkConfig.MaxLinkRate<0x0aU) {
        s.reason="SINK_REQUIRES_DIFFERENT_LINK_MODE"; return -1;
    }
    if (edid_supported()!=0) { return -1; }
    if (XDpPsu_SetEnhancedFrameMode(&dp,(u8)(dp.LinkConfig.SupportEnhancedFramingMode!=0U))!=XST_SUCCESS ||
        XDpPsu_SetLaneCount(&dp,2)!=XST_SUCCESS || XDpPsu_SetLinkRate(&dp,0x0a)!=XST_SUCCESS ||
        XDpPsu_SetDownspread(&dp,(u8)(dp.LinkConfig.SupportDownspreadControl!=0U))!=XST_SUCCESS) {
        s.reason="LINK_CONFIG_FAILED"; return -1;
    }
    /* Explicit, foreground attempt only; no retraining inside ISR or poll. */
    usleep(100000U);
    if (XDpPsu_EstablishLink(&dp)!=XST_SUCCESS) { s.reason="LINK_TRAIN_FAILED"; return -1; }
    if (dp.LinkConfig.LaneCount!=2U || dp.LinkConfig.LinkRate!=0x0aU) {
        s.reason="LINK_FELL_BACK_OUTSIDE_FIXED_MODE"; return -1;
    }
    paint_once();
    XAVBuf_EnableGraphicsBuffers(&av,1);
    XDpPsu_SetColorEncode(&dp,XDPPSU_CENC_RGB);
    XDpPsu_CfgMsaSetBpc(&dp,XVIDC_BPC_8);
    XDpPsu_CfgMsaUseStandardVideoMode(&dp,XVIDC_VM_1920x1080_60_P);
    if (dp.MsaConfig.PixelClockHz != s.pixel_hz) {
        s.reason="STANDARD_MSA_CLOCK_MISMATCH"; return -1;
    }
    if (protected_clocks_unchanged()!=0) { return -1; }
    XDpPsu_WriteReg(dp.Config.BaseAddr,XDPPSU_SOFT_RESET,1U);
    usleep(10U);
    XDpPsu_WriteReg(dp.Config.BaseAddr,XDPPSU_SOFT_RESET,0U);
    XDpPsu_SetMsaValues(&dp);
    /* dppsu 1.7 writes an extra *1000 into this 10-bit PS register.
     * Use the PS hardware's thousandths directly, as the official
     * linux-xlnx zynqmp_dp.c path does. Keep the BSP source unchanged. */
    XDpPsu_WriteReg(dp.Config.BaseAddr,XDPPSU_FRAC_BYTES_PER_TU,dp.MsaConfig.AvgBytesPerTU%1000U);
    if(XDpPsu_ReadReg(dp.Config.BaseAddr,XDPPSU_FRAC_BYTES_PER_TU)!=dp.MsaConfig.AvgBytesPerTU%1000U){
        s.reason="PS_TU_FRACTION_READBACK_FAILED";return -1;
    }
    DISPLAY_LOG("[DISPLAY] PS TU min=%u frac_per_1000=%u readback=%u\r\n",dp.MsaConfig.AvgBytesPerTU/1000U,
        dp.MsaConfig.AvgBytesPerTU%1000U,XDpPsu_ReadReg(dp.Config.BaseAddr,XDPPSU_FRAC_BYTES_PER_TU));
    XDpPsu_WriteReg(dp.Config.BaseAddr,XAVBUF_BUF_SRST_REG,3U);
    usleep(10U);
    XDpPsu_WriteReg(dp.Config.BaseAddr,XAVBUF_BUF_SRST_REG,0U);
    XDpDma_WriteReg(dma.Config.BaseAddr,XDPDMA_ISR,0xffffffffU);
    XDpDma_WriteReg(dma.Config.BaseAddr,XDPDMA_EISR,0xffffffffU);
    s.isr_errors=0U; s.eisr_errors=0U;
    queue_frame(0U);
    XDpPsu_EnableMainLink(&dp,1);
    s.active=1;
    s.reason="LINK_UP_WAITING_FOR_DPDMA";
    DISPLAY_LOG("[DISPLAY] link trained: 1080p 1920x1080 60Hz RGBA8888, 2 lanes HBR; waiting for hardware descriptor acknowledgement.\r\n");
    return 0;
}
void radar_display_init(RadarConsole *console)
{
    display_console=console;
    memset(&s,0,sizeof s);
    s.front=-1; s.pending=-1; s.reason="OFF_NOT_INITIALIZED";
    radar_view_schedule_init(&s.view);
    radar_view_cache_init(&view_cache,(RadarViewSnapshot *)(UINTPTR)RADAR_DSP_VIEW_SLOT0,
                          (RadarViewSnapshot *)(UINTPTR)RADAR_DSP_VIEW_SLOT1);
}
void radar_display_scope_update(const RadarRangeFrame *history, unsigned count,
                                const char *status, u32 revision,
                                int scan_active, const RadarViewDiagnostics *diagnostics,
                                const RadarSpatialFrame *spatial)
{
    radar_display_scope_scene_update(history,count,status,revision,scan_active,diagnostics,spatial,NULL);
}
void radar_display_scope_scene_update(const RadarRangeFrame *history,unsigned count,
    const char *status,u32 revision,int scan_active,const RadarViewDiagnostics *diagnostics,
    const RadarSpatialFrame *spatial,const RadarSceneFrame *scene)
{
    s.scope_scan_active=scan_active;
    if(diagnostics) s.diagnostics=*diagnostics;
    if (revision==0U || revision==s.view.requested.data_revision) { return; }
    if(radar_view_cache_publish(&view_cache,history,count,spatial,&s.diagnostics,status,revision)) {
        DISPLAY_LOG("[DISPLAY] immutable snapshot rejected; prior display retained.\r\n");return;
    }
    if(scene)view_cache.slot[view_cache.latest]->scene=*scene;
    s.scope_history=view_cache.slot[view_cache.latest]->history;
    s.scope_spatial=&view_cache.slot[view_cache.latest]->spatial;
    s.scope_count=count;
    s.scope_status=view_cache.slot[view_cache.latest]->status;
    radar_view_schedule_data(&s.view,revision);
    s.scope_enabled=1;
}
void radar_display_request_page(RadarViewPage page)
{
    if((unsigned)page>=RADAR_VIEW_COUNT) return;
    radar_view_schedule_page(&s.view,page);
    s.scope_enabled=1;
}
RadarViewPage radar_display_get_requested_page(void)
{
    return s.view.requested.page;
}
int radar_display_view_command(const char *command)
{
    unsigned i;
    if(strncmp(command,"display view",12U)!=0 ||
       (command[12]!=' ' && command[12]!='\0')) return 0;
    if(command[12]==' '){
        for(i=0U;i<RADAR_VIEW_COUNT;++i){
            if(!strcmp(command+13U,radar_view_page_name((RadarViewPage)i))){
                radar_display_request_page((RadarViewPage)i);
                DISPLAY_LOG("[VIEW] requested=%s revision=%u; %s\r\n",
                    radar_view_page_name(s.view.requested.page),s.view.requested.view_revision,
                    s.scope_scan_active?"applies with next normal frame":"applies at next idle display opportunity");
                return 1;
            }
        }
    }
    DISPLAY_LOG("[VIEW] display view overview|azimuth|top|space; requested=%s presented=%s\r\n",
        radar_view_page_name(s.view.requested.page),
        s.view.presented.view_revision?radar_view_page_name(s.view.presented.page):"none");
    return 1;
}
u32 radar_display_scope_presented(void)
{
    return s.active && !s.fault ? s.view.presented.data_revision : 0U;
}
const RadarTimingAck *radar_display_last_ack(void)
{
    return &s.last_ack;
}
u32 radar_display_faults(void)
{ return (s.fault?1U:0U)|(s.isr_errors?2U:0U)|(s.eisr_errors?4U:0U)|(s.tx_fifo_overflow?8U:0U); }
int radar_display_active(void)
{
    return s.active && !s.fault;
}
int radar_display_monitor_present(void)
{
    return (XDpPsu_ReadReg(XPAR_XDPPSU_0_BASEADDR,XDPPSU_INTERRUPT_SIG_STATE)&
            XDPPSU_INTERRUPT_SIG_STATE_HPD_STATE_MASK)!=0U;
}
int radar_display_ready(void)
{
    return radar_display_active() && !(radar_display_faults()&7U) && s.front>=0;
}
u32 radar_display_fifo_observations(void){return s.tx_fifo_observations;}
void radar_display_poll(int capture_busy)
{
    u32 id;
    if (!s.configured || !s.active) { return; }
    /* Latch DMA diagnostics even while busy, but never touch radar state. */
    s.isr_errors|=XDpDma_ReadReg(dma.Config.BaseAddr,XDPDMA_ISR)&DMA_ERRORS;
    s.eisr_errors|=XDpDma_ReadReg(dma.Config.BaseAddr,XDPDMA_EISR);
    if (s.isr_errors!=0U || s.eisr_errors!=0U) {
        if (capture_busy) { return; }
        s.reason="DPDMA_ERROR";
        DISPLAY_LOG("[DISPLAY] DPDMA error ISR=%08x EISR=%08x; radar unchanged.\r\n",s.isr_errors,s.eisr_errors);
        (void)stop_graphics();
        return;
    }
    if (s.hpd!=0U && !capture_busy) {
        u32 connected=(u32)XDpPsu_IsConnected(&dp);
        u32 link_rc=connected?XDpPsu_CheckLinkStatus(&dp,dp.LinkConfig.LaneCount):XST_DEVICE_NOT_FOUND;
        s.hpd=0U;
        if (!connected || link_rc!=XST_SUCCESS) {
            u8 lane_status[6]={0U};
            u32 aux_rc=connected ? XDpPsu_AuxRead(&dp,0x202U,6U,lane_status) : XST_DEVICE_NOT_FOUND;
            DISPLAY_LOG("[DISPLAY] link event HPD=%u check_rc=%u DP_irq=%08x TX_fifo=%08x\r\n",connected,link_rc,s.dp_irq_latched,s.tx_fifo_overflow);
            /* Diagnostic only, before disabling MainLink. The cached read
             * may be stale if CheckLinkStatus failed its AUX transaction. */
            DISPLAY_LOG("[DISPLAY LINK DETAIL] cached=%02x%02x%02x%02x%02x%02x aux_rc=%u before_stop=%02x%02x%02x%02x%02x%02x\r\n",
                dp.RxConfig.LaneStatusAdjReqs[0],dp.RxConfig.LaneStatusAdjReqs[1],
                dp.RxConfig.LaneStatusAdjReqs[2],dp.RxConfig.LaneStatusAdjReqs[3],
                dp.RxConfig.LaneStatusAdjReqs[4],dp.RxConfig.LaneStatusAdjReqs[5],aux_rc,
                lane_status[0],lane_status[1],lane_status[2],lane_status[3],lane_status[4],lane_status[5]);
            s.reason="LINK_CHANGED_RUN_DISPLAY_INIT";
            (void)stop_graphics();
            DISPLAY_LOG("[DISPLAY] link changed; explicit display init required. Radar unchanged.\r\n");
            return;
        }
    }
    if (s.pending<0) {
        if (capture_busy) { return; }
        if (s.scope_enabled && radar_view_schedule_needs_render(&s.view,s.scope_scan_active) && s.front>=0) {
            const RadarViewSnapshot *snapshot=radar_view_cache_acquire(&view_cache);
            unsigned back=(unsigned)(s.front^1);
            u32 *pixels=(u32 *)radar_memory_display_frame(back);
            RadarViewDiagnostics diagnostics;
            if(!snapshot)return;
            if(snapshot->revision!=s.view.requested.data_revision) {
                (void)radar_view_cache_release(&view_cache,snapshot->revision);return;
            }
              radar_view_cache_diagnostics(snapshot,radar_hw_now_us(),&diagnostics);
            diagnostics.dp_active=(u32)(s.active && !s.fault);
            diagnostics.dp_isr_errors=s.isr_errors;
            diagnostics.dp_eisr_errors=s.eisr_errors;
            diagnostics.data_presented_revision=s.view.presented.data_revision;
            diagnostics.view_presented_revision=s.view.presented.view_revision;
            (void)snprintf(diagnostics.dp_state,sizeof diagnostics.dp_state,"%s",s.reason);
            /* Snapshot the page/data tuple before rendering. Requests arriving
             * after submission cannot rewrite the descriptor's identity. */
            if(!radar_view_schedule_submit(&s.view)) {
                (void)radar_view_cache_release(&view_cache,snapshot->revision);return;
            }
            memset(&s.pending_timing,0,sizeof s.pending_timing);
            s.pending_timing.frame_id=snapshot->count?snapshot->history[snapshot->count-1U].session:0U;
            s.pending_timing.data_revision=s.view.submitted.data_revision;
            s.pending_timing.view_revision=s.view.submitted.view_revision;
            s.pending_timing.page=(u32)s.view.submitted.page;
            {
                u64 render_start=radar_hw_now_us();
                s.pending_timing.render_begin_ps_us=render_start;
                radar_scope_render_scene(pixels,STRIDE/4U,snapshot->history,snapshot->count,snapshot->status,
                                   s.view.submitted.page,&diagnostics,&snapshot->spatial,&snapshot->scene);
                s.pending_timing.render_end_ps_us=radar_hw_now_us();
                s.scope_render_us=s.pending_timing.render_end_ps_us-render_start;
            }
            Xil_DCacheFlushRange((UINTPTR)pixels,STORAGE_BYTES);
            radar_hw_barrier();
            s.scope_submitted_count=s.scope_count;
            (void)radar_view_cache_release(&view_cache,snapshot->revision);
            queue_frame(back);
            s.pending_timing.queue_ps_us=s.pending_since;
        }
        return;
    }
    /* Observe an already submitted descriptor even during native UART/PHY
     * work. This is bounded MMIO/bookkeeping; rendering and AUX remain gated.
     * Delaying this observation used to block the next free display buffer. */
    id=XDpDma_ReadReg(dma.Config.BaseAddr,XDPDMA_CH3_DSCR_ID)&XDPDMA_CH_DSCR_ID_VAL_MASK;
    if (s.request==0U && s.submitted!=0U && id==s.pending_id &&
        (unsigned)(s.vsync-s.submitted)>=2U) {
        s.front=s.pending;
        s.pending=-1;
        if (radar_view_schedule_ack(&s.view)) {
            if(++s.ack_sequence==0U)++s.ack_sequence;
            s.pending_timing.sequence=s.ack_sequence;
            s.pending_timing.ack_ps_us=radar_hw_now_us();
            s.pending_timing.coalesced=view_cache.coalesced;
            s.last_ack=s.pending_timing;
            s.reason="SCANNING_RADAR_SCOPE";
            if(!live_quiet)DISPLAY_LOG("[SCOPE DISPLAY] revision=%u frames=%u view=%s view_revision=%u buffer=%u descriptor=%u vsync=%u render_us=%llu\r\n",
                s.view.presented.data_revision,s.scope_submitted_count,
                radar_view_page_name(s.view.presented.page),s.view.presented.view_revision,
                (unsigned)s.front+1U,(unsigned)id,s.vsync,(unsigned long long)s.scope_render_us);
        } else {
            s.reason="SCANNING_STATIC_TEST_IMAGE";
            DISPLAY_LOG("[DISPLAY] scanning page=%u descriptor=%u vsync=%u; monitor image still requires visual confirmation.\r\n",
                       (unsigned)s.front+1U,(unsigned)id,s.vsync);
        }
    } else if (radar_hw_now_us()-s.pending_since>1000000U) {
        if (capture_busy) { return; }
        s.reason="DPDMA_ACK_TIMEOUT";
        (void)stop_graphics();
        DISPLAY_LOG("[DISPLAY] descriptor acknowledgement timed out; radar unchanged.\r\n");
    }
}
static void status_display(void)
{
    const RadarRangeFrame *range=s.scope_history&&s.scope_count?&s.scope_history[s.scope_count-1U]:NULL;
    RadarRangeQuality quality=radar_range_assess(range,s.scope_spatial);
    DISPLAY_LOG("[DISPLAY] active=%d state=%s front=%d pending=%d vsync=%u HPD_pending=%u EDID_blocks=%u\r\n",
               s.active,s.reason,s.front,s.pending,s.vsync,s.hpd,s.edid_blocks);
    DISPLAY_LOG("[DISPLAY] mode=CEA_1920x1080p60 pixel_nominal=%u refresh_nominal_mHz=%u Htotal=2200 Vtotal=1125 RGBA8888\r\n",
               s.pixel_hz,(u32)(((u64)s.pixel_hz*1000U)/(STANDARD_HTOTAL*STANDARD_VTOTAL)));
    DISPLAY_LOG("[DISPLAY] stride=%u line_bytes=%u frame_bytes=%u storage_bytes=%u buffers=01400000,01c00000 control=02400000\r\n",STRIDE,LINE_BYTES,FRAME_BYTES,STORAGE_BYTES);
    DISPLAY_LOG("[DISPLAY] DPDMA_ISR_errors=%08x EISR=%08x; view=%s requested=%s frames=%u data_presented=%u view_presented=%u\r\n",
        s.isr_errors,s.eisr_errors,s.scope_enabled?radar_view_page_name(s.view.presented.page):"STATIC_TEST",
        radar_view_page_name(s.view.requested.page),s.scope_count,s.view.presented.data_revision,
        s.view.presented.view_revision);
    DISPLAY_LOG("[VIEW DATA] cached_us=%llu result_matches_history=%u result_session=%u bytes=%u history_session=%u\r\n",
        (unsigned long long)s.diagnostics.sampled_us,s.diagnostics.last_result_valid,
        s.diagnostics.last_session,s.diagnostics.last_bytes,
        s.scope_count?s.scope_history[s.scope_count-1U].session:0U);
    DISPLAY_LOG("[VIEW QUEUE] published=%u rendered=%u coalesced=%u; current_time_trusted=%u\r\n",
        view_cache.published,view_cache.rendered,view_cache.coalesced,range?!range->time_untrusted:0U);
    DISPLAY_LOG("[VIEW RANGE] session=%u confidence=%s distance_valid=%u raw_peak_bin=%u displayed_mm=%d; peak check only, not calibrated target identity\r\n",
        range?(unsigned)range->session:0U,radar_range_quality_name(quality),
        (unsigned)(quality==RADAR_RANGE_QUALITY_QUALIFIED),range?(unsigned)range->peak_bin:0U,
        quality==RADAR_RANGE_QUALITY_QUALIFIED?(int)((double)range->peak_bin*RADAR_RANGE_BIN_METERS*1000.0+0.5):-1);
}
static void diagnostic_display(void)
{
    static const u32 offsets[]={0x84U,0x110U,0x180U,0x184U,0x188U,0x18cU,0x190U,0x194U,
        0x198U,0x19cU,0x1a0U,0x1a4U,0x1a8U,0x1acU,0x1b0U,0x1b4U,0x1b8U,
        0x1bcU,0x1c4U,0x1c8U,0x1ccU,0x280U,0xa00cU,0xa014U,0xa018U,0xa01cU,
        0xb00cU,0xb01cU,0xb070U,0xb120U,0xb124U};
    static const u32 sink_addr[]={0x100U,0x200U,0x210U};
    static const char hex[]="0123456789abcdef";
    unsigned i,j;
    char b[65];
    if(!s.configured){DISPLAY_LOG("[DISPLAY DIAG] Not initialized.\r\n");return;}
    DISPLAY_LOG("[DISPLAY DIAG] HPD=%u lanes=%u link_rate=%02x pixel=%u errors=%08x/%08x\r\n",
        (unsigned)XDpPsu_IsConnected(&dp),(unsigned)dp.LinkConfig.LaneCount,
        (unsigned)dp.LinkConfig.LinkRate,s.pixel_hz,s.isr_errors,s.eisr_errors);
    DISPLAY_LOG("[DISPLAY DIAG] DP_irq_latched=%08x TX_fifo_latched=%08x\r\n",s.dp_irq_latched,s.tx_fifo_overflow);
    DISPLAY_LOG("[DISPLAY DIAG] TX_fifo_observations=%u (ISR observations, includes startup)\r\n",s.tx_fifo_observations);
    for(i=0;i<sizeof(offsets)/sizeof(offsets[0]);++i){
        DISPLAY_LOG("[DISPLAY REG] %04x=%08x\r\n",offsets[i],XDpPsu_ReadReg(dp.Config.BaseAddr,offsets[i]));
    }
    for(i=0;i<3U;++i){
        u8 v[8]={0};u32 rc=XDpPsu_AuxRead(&dp,sink_addr[i],8U,v);
        for(j=0;j<8U;++j){b[2U*j]=hex[v[j]>>4];b[2U*j+1U]=hex[v[j]&15U];}b[16]='\0';
        DISPLAY_LOG("[DISPLAY DPCD] %03x rc=%u data=%s\r\n",sink_addr[i],rc,b);
    }
    for(i=0;i<saved_edid_bytes;i+=32U){
        for(j=0;j<32U;++j){u8 v=saved_edid[i+j];b[2U*j]=hex[v>>4];b[2U*j+1U]=hex[v&15U];}b[64]='\0';
        DISPLAY_LOG("[DISPLAY EDID] %03x %s\r\n",i,b);
    }
    DISPLAY_LOG("[DISPLAY DIAG END]\r\n");
}
int radar_display_command(const char *command,int capture_busy)
{
    if(radar_display_view_command(command)) return 1;
    if (strncmp(command,"display",7)!=0 || (command[7]!=' ' && command[7]!='\0')) { return 0; }
    if (strcmp(command,"display status")==0) { status_display(); return 1; }
    if (strcmp(command,"display")==0) {
        DISPLAY_LOG("display init | display test | display status | display off | display view overview|azimuth|top|space\r\n"); return 1;
    }
    if (capture_busy) {
        DISPLAY_LOG("[DISPLAY] BUSY: capture/control transaction owns the runtime; existing image stays static.\r\n"); return 1;
    }
    if (strcmp(command,"display diag")==0) { diagnostic_display(); return 1; }
    if (strcmp(command,"display init")==0) {
        if (s.active) { DISPLAY_LOG("[DISPLAY] already active; use display test/status/off.\r\n"); }
        else if (start_display()!=0) {
            (void)stop_graphics();
            DISPLAY_LOG("[DISPLAY] init failed: %s; radar unchanged.\r\n",s.reason);
        }
    } else if (strcmp(command,"display test")==0) {
        if (!s.active) { DISPLAY_LOG("[DISPLAY] run display init first.\r\n"); }
        else if (s.pending>=0) { DISPLAY_LOG("[DISPLAY] frame switch pending; run display status.\r\n"); }
        else {
            unsigned back=(unsigned)(s.front^1);
            s.scope_enabled=0;
            radar_view_schedule_forget_presentation(&s.view);
            paint_test_page(back);
            queue_frame(back);
        }
    } else if (strcmp(command,"display off")==0) {
        if (stop_graphics()==0) { s.reason="OFF"; }
        status_display();
    } else { DISPLAY_LOG("[DISPLAY] unknown command: use display init/test/status/off.\r\n"); }
    return 1;
}
