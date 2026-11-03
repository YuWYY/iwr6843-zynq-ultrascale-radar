# PL DSP integration contract — revision 1

Build: 20261101_pl_dsp_01. Offline only. Baseline is frozen in
/archive/20261101_pl_dsp_01.

## Ownership

Root owns native BD integration, AXI-Lite wrapper, full native builds and evidence.
DSP agent owns rtl/dsp/radar_dsp_engine.sv and FFT/transpose/power/CFAR helpers,
new FFT IP generation Tcl and datapath simulations. PS agent owns production C,
linker, capture/frame queues, display ownership, related host tests.
Algorithm agent owns temporal model/tests and rtl/dsp/radar_temporal_detect.sv.
Do not edit another owner's files without messaging first.

## Fixed geometry and format

96 chirps, TX slots [0,2,1], four RX, 256 samples, 32 loops.
Input job I/Q: 98304 complex values in [chirp][rx][sample], each value is a
64-bit word: low32 is sign-extended raw signed16 I, high32 signed16 Q.
Input bytes=786432. Raw HSI transport remains separate and unchanged (399360 B).
Compute signed24 Q2.22: raw signed16 <<7, subtract 256-sample mean, Hann256,
forward FFT scaled 1/256; Hann32 and forward FFT scaled 1/32.
Window coefficients use unsigned Q0.24 with 1.0 represented by 0x1000000
(25-bit storage). Round to nearest, ties to even; count saturation/errors.
FFT natural ordering, explicit 48-bit native packing vs 64-bit DDR format.
Both native FFT IPs use the approved Radix-2 Burst I/O architecture, C_ARCH=2,
nonrealtime backpressure. Every radix-2 stage shifts by1: Range8stages/Doppler5.
Generated config widths are24/16bits; forward is bit0, SCALE_SCH begins atbit1.
The verified config words are24'h00AAAB and16'h02AB. Earlier streaming configs
0x0155/0x35 and C_ARCH3 are superseded evidence, not this implementation.
Each output component is signed24 sign-extended into its own 32-bit DDR slot.
Range scratch [chirp][rx][range]. Final uncalibrated cube [range][physicalTX*4+rx][doppler].
Spectrum: 256 unsigned64 sums of |rangeFFT|^2 across all384 vectors.
RD power: 8192 unsigned64 sums of |dopplerFFT|^2 across12 channels.
Conversion: range spectrum linear=integer_sum/2^44*(256/127.5)^2/384.
Cube for existing PS spatial code=integer_complex/2^22*(256/127.5)*32.
RD linear=integer_sum/2^44*(256/127.5)^2*(32/15.5)^2/12.
Hann definitions use N-1 denominator as current production software.
Candidates retain original threshold factor15.8489319246, range6..204,
Doppler wrapping, 3x3 NMS and exact ties/top32 ordering. Header64B then up to32
records of32B: u32 range,u32 unsigned_doppler,u64 raw_power,u64 noise_sum,
u32 noise_count,u32 flags. Header: count,total,truncated,error followed counters.

## CSR and job interface

AXI-Lite0xA0020000/64KiB. 32-bit registers; byte strobes respected.
00 magic0x52445031;04 ABI0x00010000;08 control START=1 CANCEL=2 CLEAR=4 RECOVER=8;
0c status BUSY=1 DONE=2 ERROR=4;10 error;14 mode(1 baseline,2 enhanced);
18 job_id;1c config_epoch;20 input;24 range_tmp;28 cube_out;2c rd_out;
30 spectrum_out;34 candidates_out;38 temporal_state;3c input_bytes;
40 timestamp_us_low;44 timestamp_us_high;48 frame_index;4c acquisition_id;
50 validity_flags (HSI=1 CRC=2 trusted_timestamp=4 clipped=8 discontinuity=16 TIME_TRANSPORT=32);
54 alpha_q31;58 temporal_control;5c baseline_threshold_q16;
60 enhanced_threshold_q16;64 hold_threshold_q16;
68 cycles_low;6c cycles_high;70 candidate_count;74 overflow_count;
78 vector_count;7c result_flags. Successful vector_count is3456 total native
FFT vectors:384 Range transforms plus3072 Doppler transforms (not384).
Root wrapper freezes 32-register cfg bus [1023:0] at START, cfg[word*32+:32].
radar_dsp_engine ports: clk,rst_n,start,cancel,cfg[1023:0];
outputs busy,done,error[31:0],cycles[63:0],candidate_count[31:0],
overflow_count[31:0],vector_count[31:0],result_flags[31:0].
Memory ports (prefixes mm2s/s2mm): cmd_tdata[71:0],cmd_tvalid,cmd_tready;
sts_tdata[7:0],sts_tvalid,sts_tready. MM2S data input: mm2s_tdata[63:0],
mm2s_tvalid,mm2s_tready,mm2s_tlast. S2MM data output uses s2mm_ equivalents.
DataMover command bits: tag[67:64],addr[63:32],DRR[31],EOF[30],DSA[29:24],
INCR[23],BTT[22:0]. Writes complete only after successful S2MM status.
Cancellation drains accepted memory commands before error completion.
Fatal DataMover errors soft-halt both directions before returning error bit31
LOCKED; the engine rejects another job until explicit recovery. RECOVER8 is
accepted only while idle, holds the engine AND dedicated DataMover reset for
16 PL0 cycles, and reports BUSY during that interval. It never resets capture
DMA, the metadata clock/queue, AXI-Lite registers, or other system peripherals.
CLEAR4 clears only the latched result and cannot unlock the engine. A host
timeout while BUSY is not permission to reset an outstanding AXI transaction.
The engine additionally drives mm2s_halt/s2mm_halt and receives each matching
halt_cmplt. Any DataMover/protocol error enters soft shutdown, drains MM2S
residual payload, and only reports terminal error after BOTH halt completions.
Error bit31 means LOCKED; further START cannot run until root's dedicated
DataMover+engine common16-cycle reset (CSR control bit3 RECOVER, idle only).
Normal cancellation does not invoke this destructive recovery.

## Memory / runtime

Dedicated WB workspace [0x02600000,0x03600000), no increase of ordinary BSS
by large arrays. Old RX, raw, DP and descriptor ranges stay fixed.
All addresses/capacities checked against workspace;64-byte aligned transfers.
Driver flushes inputs, invalidates outputs only after DONE; no silent fallback.
Polling bounded and cooperative; no board operations in this work.
Existing four pages and KEY3 stay visually unchanged; new backend results
are imported once, no duplicate Doppler or scale/TDM application.

## Transport timestamp metadata CSR extension

Root radar_dsp_accel passively observes FIFO M_AXIS handshakes in PL0 clock;
no extra capture backpressure. 64 records, retained across DMA reset. Sources
are TRANSPORT_SYS_FIRST_BEAT / LAST_BEAT, NOT ADC sampling or RF emission time.
Clock frequency register0x124=99999001Hz. Only SYS reset clears the tick clock.
0x100 status: bits6:0 count,bit31 overflow sticky,bit30 frame-active,
bit29 clear-raced-an-accepted-beat.0x104 completed transport count (including drops);
head fields0x108 seq,0x10c/110 first tick,0x114/118 last tick,0x120 bytes.
0x11c write1 POP,write2 CLEAR queue/sequence/partial frame (clock keeps running).
Never pop until all fields read; producer never changes head while nonempty.
Before ARM explicitly clear; snapshot and bind each matching byte-count record
by transport sequence after complete DMA frame, then HSI/CRC validate separately.
Recorded transport sequence is one-based (1..N after CLEAR), matching DMA
packet IDs. The separate FrameRef frame_index remains zero-based within a batch.
Overflow/race/missing metadata makes time untrusted; do not synthesize100ms.
These timestamps are eligible for noncoherent time processing only with an
explicit transport-observed source label; future board tests must quantify
FIFO/DDR queuing influence. They never establish cross-frame RF phase coherence.

## Temporal block contract

## PS workspace layout (fixed offsets within the reserved region)

PS temporal reset ownership: RTL validates a prior header's time/source identity
before epoch/manual reset. After submit passes source-duplicate and hardware-idle
capability checks, PS clears and flushes the64-byte temporal header before an
enhanced reset (initial/sticky recovery, explicit manual reset, or new config
epoch). The engine initializes cells. An intervening baseline retains the
pending reset. Software source identity is never cleared by this operation.
Normal same-epoch submissions keep the header and its time-regression checks.

PS sampled-result freshness is separate from the PL background-protection
state. Immutable publication metadata stores source identity, backend, result
flags, timestamp source/origin, source/processing validity and a500000us cap.
CURRENT is only a time-eligibility classification with an explicitly trusted
same-domain now; it is not target confidence, enhanced-mode availability, or a
protection renewal. Repeated snapshots/queries do not change the cap. The
current application lacks PL-to-PS clock correspondence, so its serial status
reports SAMPLED_ONLY and never CURRENT. PS DMA-observation age is a lower bound:
greater than500000us proves EXPIRED, less cannot prove CURRENT. Bad integrity,
clipping/discontinuity or numerical errors never gain current eligibility.
The UI keeps its sampled-frame labels without automatic expiry redraw.

Input02600000/786432B; range scratch026C0000/786432B;
cube02780000/786432B; RD02840000/65536B; spectrum02850000/2048B;
candidates02850800/1088B; temporal02851000/524352B.
Immutable display snapshots use02A00000 and02A80000, each512KiB.
No DMA job may overlap those display snapshots. Generated hardware identity
defaults absent; PL selection additionally requires the matching BSP address
and CSR magic/ABI. The ordinary program remains below00200000.

PS production finite pipeline uses independent RX and RAW ownership. The
consumer leases a detached immutable completed RAW batch while the next finite
producer qualifies/configures/captures into RX. Native sensorStop and payload
HALT precede COPY; COPY waits for RAW release and fails boundedly after30s,
without overwriting either region or blindly resetting DMA. Original default
single-frame gated path remains. Explicit pipeline on requires batch8/16 plus
display decoupled. At most one next RX batch is pending; this is not unlimitedRF.

PS parameter version TEMPORAL_V1_HOLD1P5_CFAR12DB uses holdQ16=98304,
baseline/enhanced detection thresholdQ16=1038676. Enhanced default is experimental,
not a threshold selected as a claimed measured gain. Alpha is nonnegative RNE
of -expm1(-trusted_dt_us/2000000)*2^31, clamped0..0x80000000. Unknown timestamps
never replace the trusted-dt reference. The DSP driver retains the last submitted
source(acquisition_id,frame_index) across temporal resets and rejects duplicate
or backwards source identity before another START. TIME_UNTRUSTED bit10 permits
explicit raw-RD fallback while preserving the unavailable-time label.

## Temporal block contract (continued)

Use integer RD power, independent from cross-frame complex phase.
At most four fresh valid frames within500000us; reject duplicates/bad identity.
Background tau2s, supplied alpha_q31; protection expires after500000us.
Only fresh local evidence renews protection; old/predicted data cannot renew.
State per RD cell is64B: background at0, history[4] at8..32,
protected_until_us at40, reserved/flags at48..63. Global history/times are
managed in a64B state header; warmup and untrusted/sparse inputs explicitly flagged.
Algorithm agent must coordinate the exact streaming block ports with DSP agent
before connecting it; output gating must never label stale results current.

## Evidence gates

Native vendor models required for FFT simulation. Bit-exact model vs RTL;
float FS-normalized complex RMS<=1e-5, spectrum bins>-80dBFS error<=0.1dB.
Upgrade uniformly to32 if24 fails; do not weaken thresholds.
Weak-target improvement is experimental; preserve negative results and do not
claim superiority to TI or physical performance from simulations.

## Temporal mathematics and streaming ABI — revision 1

Module `radar_temporal_detect`, parameter `CELL_COUNT=8192`, ports:
`clk,rst_n,cancel,start_frame`; outputs `frame_ready,frame_header_valid`;
inputs `frame_header_in[511:0],timestamp_us[63:0],config_epoch[31:0],
acquisition_id[31:0],frame_index[31:0],validity_flags[31:0],alpha_q31[31:0],
temporal_control[31:0],hold_threshold_q16[31:0]`;
outputs `frame_header_out[511:0],frame_flags[31:0]`;
inputs `cell_valid,cell_index[12:0],power[63:0],state_in[511:0],cell_local_max`;
output `cell_ready`;
outputs `result_valid,result_index[12:0],state_out[511:0],
enhanced_power[63:0],fresh_evidence,result_flags[31:0]`;
input `result_ready`.

An accepted start produces a one-cycle frame_header_valid pulse; returned
header/flags remain stable. Cells are processed in ascending order 0..8191.
Result payload remains stable until accepted. frame_ready returns only after
the final cell result is accepted. cancel abandons the current transaction.
Engine must invalidate the external header before the first in-place cell write,
and commit the new header only after every cell write has completed successfully;
otherwise cancellation can combine a previous header with partially new cells.

Header is little-endian 64B: u32 magic TMP1=0x544d5031 at0, u32 config_epoch at4,
u32 acquisition_id at8, u32 frame_index at12, u64 last_timestamp_us at16,
four u64 history timestamps at24,32,40,48; byte56 valid slot mask (low4 bits),
byte57 next write slot (0..3), byte58 warmup count (0..4), other bytes zero.
Cell is u64 background at0, four u64 history powers at8,16,24,32,
u64 protected_until_us at40, and 16 zero reserved bytes. Ring slots correspond
directly to the header timestamps; zero is a legal power and legal timestamp.

Fresh temporal input needs (validity_flags&7)==7, !(flags&24), alpha_q31<=0x80000000,
hold_threshold_q16>=65536, and temporal_control bits other than bit0 clear.
Validity bit5 TIME_TRANSPORT identifies FIFO-to-DMA first-beat timestamps; bit2
still explicitly asserts trust. This supports noncoherent timing only, not RF
frame-start timing or cross-frame phase coherence.
With HSI+CRC valid, no clipping/discontinuity, but trusted-time bit2 absent,
return raw current power with BASELINE_FALLBACK|TIME_UNTRUSTED|STATE_REINIT;
clear the header and every cell, and set fresh_evidence=0. Do not compare an
untrusted zero/unknown timestamp to a trusted timestamp. This fallback does not
establish current temporal freshness. PS guards duplicate identities across such
cleared states. A duplicate identity that is still known from an input header is
rejected even if the incoming timestamp is untrusted.
Temporal_control bit0 explicitly reinitializes state. Trusted time must strictly increase.
Identity must increase lexicographically (acquisition_id,frame_index); identical
or backwards identity is rejected. Invalid/duplicate/backwards input yields a
zero invalid header, zero candidate evidence, and cleared cell state. The next
valid frame initializes state. Configuration change initializes a fresh epoch.
Unknown/uninitialized state initializes a fresh epoch. No invalid frame may be
reported as a current baseline detection.

Each valid frame retains at most four distinct valid powers whose timestamps
are within500000us, inclusive, of the current timestamp. The oldest ring slot is
replaced by current power. Four valid initialization frames use raw-RD baseline
fallback; later frames with fewer than two eligible samples also use full-frame
raw fallback. This avoids mixing raw power and normalized map units. Sparse
operation may still update a valid background; it does not pretend to integrate.
In full-frame fallback the cell enhanced_power output is the unchanged current
raw power and no unused normalized-score overflow is reported.

Outside initialization, B is the old integer background, D=max(B,1). Current
evidence requires cell_local_max and (P<<16)>=D*hold_threshold_q16 and P>0.
cell_local_max is the original raw-RD 3x3 local maximum with original tie rules.
Only current evidence can renew protection to timestamp_us+500000 (saturating).
History alone, predictions, stale frames, or zero Doppler removal do not renew it.
During protection background is frozen. Otherwise B is updated as
RNE((B*(2^31-alpha_q31)+P*alpha_q31)/2^31); initialization uses P then learns
without protection. Alpha comes from elapsed time, tau=2000000us, calculated by
the caller, rather than from assumed frame cadence. Time gaps must remain gaps.

Enhanced map is UQ32.32 RNE(RNE(sum(eligible integer powers)/count)*2^32/D),
with unsigned64 saturation flagged. The divider is sequential. The engine runs
the same-shaped spatial CFAR/NMS with enhanced_threshold_q16 on this map, then
requires fresh_evidence. Enhanced values may select/rank candidates, but output
candidate raw_power,noise_sum,noise_count ALWAYS come from this frame's raw RD
power/training window. They remain physical-power inputs for PS dBFS/SNR.
No converted UQ32.32 score may be serialized as raw RD power.

Flags: bit0 WARMUP;1 SPARSE;2 INVALID_INPUT(discard);3 IDENTITY_RESET(discard);
4 CONFIG_RESET;5 MANUAL_RESET;6 BASELINE_FALLBACK;7 STATE_REINIT;
8 SCORE_SATURATED;9 CELL_ORDER_ERROR;10 TIME_UNTRUSTED(0x400).
Cell result flags include frame flags.
Repeated/backwards identity and cell-order failures are errors, not fallback.
All 64/66/96-bit integer intermediate arithmetic and rounding must match the
independent model. This is an experimental detector; synthetic improvement or
an FPGA build is not proof of physical gain or novelty over prior detectors.

### Engine numerical acceptance gate

The standalone temporal arithmetic reports a saturated UQ32.32 score with
SCORE_SATURATED. The production engine rejects that entire job with error
`0x303`, increments overflow_count, returns zero valid candidates, and does not
commit a valid temporal header. A flagged saturated score is not an accepted
historical sample or a normal successful result. A fatal DataMover error can
interrupt a header write; PS must retain a pending temporal reinitialization
requirement across local RECOVER and baseline jobs, applying RESET to the next
enhanced job before its history can be reused.
