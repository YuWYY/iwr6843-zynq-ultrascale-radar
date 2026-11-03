/* SPDX-License-Identifier: MIT
 * F20 M2 / SW3: one S2MM SG engine, one owner, caller-provisioned DDR.
 * No GPIO, UART, sensorStart, ARM, recovery token or synthetic data source.
 */
#ifndef RADAR_DMA_H
#define RADAR_DMA_H
#include "radar_contract.h"
#include "xaxidma.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RADAR_DMA_OFF=0, RADAR_DMA_READY, RADAR_DMA_STAGED,
    RADAR_DMA_RUNNING, RADAR_DMA_HALTING, RADAR_DMA_HALTED,
    RADAR_DMA_RESETTING, RADAR_DMA_FAULT, RADAR_DMA_HALT_TIMEOUT, RADAR_DMA_LOCKED
} RadarDmaState;
enum RadarDmaRc {
    RADAR_DMA_OK=0, RADAR_DMA_PENDING=1,
    RADAR_DMA_E_ARGUMENT=-1, RADAR_DMA_E_STATE=-2,
    RADAR_DMA_E_BSP=-3, RADAR_DMA_E_MAPPING=-4,
    RADAR_DMA_E_HARDWARE=-5, RADAR_DMA_E_DRIVER=-6,
    RADAR_DMA_E_MEMORY=-7, RADAR_DMA_E_SPACE=-8,
    RADAR_DMA_E_DESCRIPTOR=-9, RADAR_DMA_E_LENGTH=-10,
    RADAR_DMA_E_BOUNDARY=-11, RADAR_DMA_E_TIMEOUT=-12,
    RADAR_DMA_E_OWNER=-13, RADAR_DMA_E_TIMER=-14,
    RADAR_DMA_E_HALT_TIMEOUT=-15, RADAR_DMA_E_RESET_TIMEOUT=-16,
    RADAR_DMA_E_DEADLINE=-17, RADAR_DMA_E_RUN_TIMEOUT=-18,
    RADAR_DMA_E_PAYLOAD_TIMEOUT=-19
};
typedef enum {
    RADAR_BUFFER_FREE=0, RADAR_BUFFER_DMA, RADAR_BUFFER_READY,
    RADAR_BUFFER_CPU
} RadarBufferOwner;

typedef struct {
    u32 capacity, actual, status, packet_sequence;
    u8 owner, first, last;
} RadarDmaSlot;
/* A token is valid only for the oldest acquired packet, in this ring epoch.
 * Data is raw transport bytes, NOT an assertion about PL/ADC validity. */
typedef struct {
    u32 epoch, sequence, session, bytes, first_slot, slot_count;
    u64 observed_us; /* first software observation of complete, validated BDs */
} RadarDmaPacket;
typedef struct { UINTPTR address; u32 bytes; } RadarDmaFragment;
typedef enum {
    RADAR_DMA_PHASE_NOT_RUN=0, RADAR_DMA_PHASE_PENDING,
    RADAR_DMA_PHASE_PASS, RADAR_DMA_PHASE_LATE, RADAR_DMA_PHASE_TIMEOUT,
    RADAR_DMA_PHASE_ERROR
} RadarDmaPhaseResult;
#define RADAR_DMA_TRACE_DEPTH 16U
#define RADAR_DMA_TRANSITION_US 10000U /* application budget, not IP promise */
typedef struct {
    u32 address, slot, next_lo, next_hi, buffer_lo, buffer_hi, control, status;
    u8 valid, owner;
} RadarDmaBdView;
typedef struct {
    u32 control, status, current, tail, bd_status;
    u32 free_count, pre_count, hw_count, post_count;
    u64 sampled_us;
    RadarDmaBdView bd[3]; /* actual BD0, BD1 and current BD; no synthetic status */
} RadarDmaRegisters;
typedef enum {
    RADAR_DMA_EVT_OPEN=1, RADAR_DMA_EVT_START, RADAR_DMA_EVT_HALT_REQUEST,
    RADAR_DMA_EVT_HALT_DONE, RADAR_DMA_EVT_HALT_TIMEOUT,
    RADAR_DMA_EVT_RESET_REQUEST, RADAR_DMA_EVT_RESET_DONE,
    RADAR_DMA_EVT_RESET_TIMEOUT, RADAR_DMA_EVT_ERROR
} RadarDmaEvent;
typedef struct {
    RadarDmaRegisters registers;
    u32 event, state, written_cr, elapsed_us;
    int error;
} RadarDmaTrace;

typedef struct {
    XAxiDma device;
    XAxiDma_BdRing *ring;
    RadarDmaState state;
    RadarDmaSlot slot[RADAR_RX_BD_COUNT];
    RadarDmaPacket packet[RADAR_RX_BD_COUNT];
    u32 packet_head, packet_tail, packet_queued;
    u32 packet_bytes, fragments_per_packet, finite_packets;
    u32 session, epoch, submitted_packets, completed_packets, released_packets;
    u32 submitted_bds, completed_bds, released_bds, partial_prefix;
    u32 stall_timeout_us, transition_polls, reset_count;
    /* max_poll_gap: active run/recovery only, reset on start or isolated reset.
     * Cumulative counters and trace history are deliberately not cleared. */
    u32 reset_requests, halt_requests, max_poll_gap_us, trace_head, trace_count;
    u32 halt_elapsed_us, reset_elapsed_us;
    u64 last_poll_us;
    RadarDmaPhaseResult halt_result, reset_result;
    RadarDmaTrace trace[RADAR_DMA_TRACE_DEPTH];
    u64 completed_bytes, started_us, last_progress_us, transition_us;
    UINTPTR bd_base, rx_base;
    int last_error, driver_error;
    RadarDmaRegisters failure;
    u8 configured, ring_created, lease_active, reset_confirmed;
    u8 stop_was_incomplete;
} RadarDma;

void radar_dma_construct(RadarDma *d);
const char *radar_dma_state_name(RadarDmaState state);
const char *radar_dma_error_name(int status);
/* Read-only MMIO. MMIO must return; no C deadline can interrupt a hung AXI bus. */
int radar_dma_probe(RadarDmaRegisters *r);
int radar_dma_registers(RadarDma *d, RadarDmaRegisters *r);
/* CfgInitialize includes a bounded hardware RESET in the supplied driver.
 * Caller MUST have exclusive DMA ownership, no active PL producer/session,
 * no external DMA owner and completed SW0 memory provisioning. */
int radar_dma_open(RadarDma *d);
/* Non-MMIO planning. 0 packets denotes a future continuous session, primed
 * with the maximum whole-packet count that fits the ring. */
int radar_dma_plan(u32 bytes, u32 packets, u32 *bds, u32 *prime_packets);
/* Stages real descriptors with RS=0. Keeps every payload until explicit release.
 * Non-Micro RX SOF/EOF belong in hardware status, NOT software control bits. */
int radar_dma_prepare(RadarDma *d, u32 bytes, u32 packets,
                      u32 session, u32 stall_timeout_us);
/* Explicit producer/consumer mode. Prime at most18 whole frames; finite total
 * can exceed the ring and MUST be replenished after release. Legacy prepare
 * continues refusing finite batches that cannot fit before consumption. */
int radar_dma_prepare_window(RadarDma *,u32 bytes,u32 total_packets,u32 session,u32 stall_timeout_us);
int radar_dma_start(RadarDma *d);
/* Call before slow console work. Bounded by 128 BDs; returns newly completed
 * packet count, zero/pending progress, or a negative error. */
int radar_dma_poll(RadarDma *d, u64 now_us);
/* FIFO-order consumer lease. Access fragments only while this exact token is
 * owned by CPU; no payload pointer may be retained after release/reset. */
int radar_dma_acquire(RadarDma *d, RadarDmaPacket *packet);
int radar_dma_fragment(RadarDma *d, const RadarDmaPacket *packet,
                       u32 fragment, RadarDmaFragment *out);
int radar_dma_release(RadarDma *d, const RadarDmaPacket *packet);
/* Queue more whole packets only from FREE slots. Needed later for continuous
 * receive; never enables cyclic overwrite. No automatic recycling. */
int radar_dma_refill(RadarDma *d, u32 packets);
/* Stop writes actual DMACR.RS=0 and waits for actual DMASR.Halted. The supplied
 * XAxiDma_Pause does NOT write RS in SG mode, so it is not used as a HW halt.
 * Stop can truncate an in-flight packet: no subsequent FromHw result is trusted. */
int radar_dma_stop_begin(RadarDma *d);
/* Destructive to queued/retained results. Caller has stopped/isolated input;
 * outstanding consumer lease refuses reset. No memory reuse until ResetIsDone
 * AND Halted. Timeout => LOCKED; no blind retry, free or requeue. */
int radar_dma_reset_begin(RadarDma *d);
/* One explicit recovery attempt from HALT_TIMEOUT (or normal owned states).
 * Caller has positively stopped/held-reset the producer, or SW3 verified the
 * source was never armed. High-Z UART alone is NOT stopped-source evidence.
 * A RESET timeout is final LOCKED; this API cannot unlock it. */
int radar_dma_reset_authorized(RadarDma *d);
int radar_dma_is_quiescent(RadarDma *d);
const char *radar_dma_phase_name(RadarDmaPhaseResult result);
int radar_dma_ring_check(RadarDma *d);

#ifdef __cplusplus
}
#endif
#endif
