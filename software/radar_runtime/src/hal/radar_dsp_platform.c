/* SPDX-License-Identifier: MIT */
#include "radar_dsp.h"
#include "radar_hw.h"
#include "xparameters.h"
#include "xil_io.h"
#include "xil_cache.h"
#include "radar_dsp_hw_identity.h"
static uint32_t read_csr(void *c,uint32_t o) { (void)c;return Xil_In32(RADAR_DSP_CSR_BASE+o); }
static void write_csr(void *c,uint32_t o,uint32_t v) { (void)c;Xil_Out32(RADAR_DSP_CSR_BASE+o,v); }
static void flush(void *c,uint32_t a,uint32_t n) { (void)c;Xil_DCacheFlushRange((UINTPTR)a,n); }
static void invalidate(void *c,uint32_t a,uint32_t n) { (void)c;Xil_DCacheInvalidateRange((UINTPTR)a,n); }
static void barrier(void *c) { (void)c;radar_hw_barrier(); }
static uint64_t now(void *c) { (void)c;return radar_hw_now_us(); }
void radar_dsp_platform_init(RadarDspDriver *d)
{
    RadarDspIo io={0};io.read=read_csr;io.write=write_csr;io.flush=flush;
    io.invalidate=invalidate;io.barrier=barrier;io.now=now;
#if RADAR_DSP_HW_PRESENT && defined(XPAR_RADAR_DSP_ACCEL_0_BASEADDR)
    io.mapped=XPAR_RADAR_DSP_ACCEL_0_BASEADDR==RADAR_DSP_CSR_BASE && RADAR_DSP_HW_BASEADDR==RADAR_DSP_CSR_BASE;
#elif RADAR_DSP_HW_PRESENT && defined(XPAR_RADAR_DSP_AXI_0_BASEADDR)
    io.mapped=XPAR_RADAR_DSP_AXI_0_BASEADDR==RADAR_DSP_CSR_BASE;
#elif RADAR_DSP_HW_PRESENT && defined(XPAR_RADAR_DSP_0_BASEADDR)
    io.mapped=XPAR_RADAR_DSP_0_BASEADDR==RADAR_DSP_CSR_BASE;
#endif
    radar_dsp_init(d,&io);
}
static RadarDspDriver transport;
int radar_dsp_transport_begin(void)
{ radar_dsp_platform_init(&transport);return radar_dsp_metadata_clear(&transport); }
int radar_dsp_transport_pop(RadarTransportTime *t)
{ return radar_dsp_metadata_pop(&transport,t); }
