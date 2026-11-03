# Architecture

Consolidated description of the `v1.12.0` design. Detailed, version-specific
contracts live next to the evidence:

- capture/RAW16 and board I/O: `fpga/docs/m2_real/`
- clock channels, reset contract and CDC review: `fpga/docs/m2_closure/`
- PL DSP register map and numerical gates: `software/verification/20261101_pl_dsp_01/CONTRACT.md`
- streaming ownership and time base: `software/verification/20261102_stream_scene_01/STREAM_CONTRACT.md`
- automatic boot sequence: `software/verification/20261103_autoboot_01/AUTOBOOT_IMPLEMENTATION.md`

---

## 1. Partitioning

| Plane | Owner | Clock | Responsibility |
|---|---|---|---|
| Data (capture) | PL | RX 75 MHz → PS 100 MHz | LVDS deserialisation, RAW16 framing, packetising, FIFO, SG DMA |
| Data (compute) | PL | PS 100 MHz (PL0) | Range/Doppler FFT, power, 2D CFAR, peak selection |
| Control | PL | SYS 100 / REF 300 / RX 75 / BITCLK 300 MHz | GPIO mailbox, ordered PHY reset, tap calibration, transaction ownership |
| Application | PS (A53) | 100 MHz | sessions, descriptors, leases, calibration, angle, scene, display |
| Boot | PS + PL | — | FSBL → bitstream → application from QSPI |

The PS never touches the LVDS pins. The PL never renders. The only software
control entry into the PL is one AXI GPIO pair; the only bulk path is AXI DMA
plus the DSP DataMover.

## 2. Clock domains and reset

| Domain | Frequency | Used by |
|---|---|---|
| SYS / PL0 | 100 MHz (99,999,001 Hz measured) | command plane, DMA, DSP engine, AXI-Lite |
| REF | 300 MHz | `IDELAYCTRL`, primitive reset sequencing, RDY qualification |
| RX | 75 MHz | `IDELAYE3` control, real tap readback, per-lane LOAD |
| BITCLK | 300 MHz DDR | real radar bit clock; `BUFGCE_DIV` /1 and /4 release |

Reset is deliberately ordered rather than timed: components are released
before the controller, `RDY` must be stable, and the 300/75 MHz dividers are
released from the same qualification. The RTL contract is 64 REF cycles of
assertion, +16 to release the primitives, +16 to release `IDELAYCTRL`, then the
full-speed clock with a three-stage synchroniser and a four-cycle guard. The
physical budget for these paths is measured on the routed design, not assumed.

Cross-domain control uses a shared-epoch mailbox: single-bit state is
registered at the source and triple-synchronised; multi-bit payloads are held
until acknowledged. Cancellation is an asynchronous assert / synchronous
release qualification shared by both ends, so a stopped clock cannot swallow
it.

## 3. Capture datapath

```
IBUFDS → IDELAYE3 → ISERDESE3 (1:8) ─┐
IBUFDS → IDELAYE3 → ISERDESE3 (1:8) ─┴→ RAW16 word align → 64-bit AXIS
   → XPM async FIFO 8192×64 → AXI DMA S2MM (SG, 128 BD) → DDR
```

- One radar frame = 96 chirps × (64 B HSI + 4,096 B ADC) = **399,360 B**, i.e.
  exactly seven 64 KiB descriptors.
- `TLAST` marks a transport packet, **not** a chirp, HSI burst or radar frame.
- Software treats a frame as valid only when source completion, FIFO drain, DMA
  halt and session closure all hold. “Source complete” is never reported as
  “capture complete”.

## 4. Control-plane ABI

`AXI GPIO` channel 1 (software → PL) and channel 2 (PL → software) carry a
versioned mailbox, ABI `0x52430101`:

```
command : [31] toggle | [30:24] opcode | [23:16] index | [15:0] argument
response: [31] ACK    | [30] error  | [29:24] result | [23:16] index | [15:0] value
```

Opcodes: `SAFE`, `IO_SET`, `CFG_LO/HI`, `READ`, `ARM`, `STOP`, `ABORT`,
`RECOVER`, `SNAPSHOT`, `DELAY_APPLY`, `RECAL`. Every command must flip the
toggle bit; the software waits for a matching ACK before issuing the next one.
Status pages expose flags, sticky faults, timestamps, PHY qualification, tap
readback and the configuration shadow registers. See
`fpga/docs/m2_real/CONTROL_ABI.md`.

## 5. PL DSP engine

`radar_dsp_accel` wraps `radar_dsp_engine` with an AXI-Lite CSR and the
DataMover streams.

- CSR base `0xA0020000`, 64 KiB, magic `0x52445031`, ABI `0x00010000`.
- Job: fixed geometry 96 chirps × 4 RX × 256 samples (98,304 complex values,
  786,432 B), one 1024-bit configuration word frozen at `START`.
- Arithmetic: signed-16 → signed-24 (`<<7`), mean removal, Hann window,
  256-point range FFT, DDR tile transpose, 32-point Doppler FFT, 12-channel
  power merge, 2D CFAR, 3×3 NMS, top-32 candidates.
- FFT cores: AMD XFFT, Radix-2 Burst, natural order, 24-bit range / 16-bit
  Doppler configuration, scaled 1/256 and 1/32 respectively.
- Outputs: range spectrum (256 bins), RD power (8,192 cells), complex cube for
  the PS angle code, and a candidate list (64 B header + ≤32 × 32 B).
- Fault handling: any memory error soft-halts both DataMover directions, drains
  accepted commands and latches `LOCKED`; only an explicit `RECOVER` while idle
  resets the engine and its DataMover for 16 cycles.
- Timing: 5,535,727 cycles ≈ 55.36 ms per job at 99.999 MHz (memory-model
  simulation; real DDR arbitration is separate).

The PS can select `PS_REFERENCE`, `PL_BASELINE` or the experimental
`PL_ENHANCED`. A mismatch between the generated hardware identity, the BSP
address and the CSR magic refuses PL use — there is no silent fallback.

## 6. Software structure

```
app/        main, console, keys, autoboot state machine
capture/    session, stream, profiles, RAW slots, export, CRC32
hal/        GPIO control, DMA, memory, clock, DSP driver, time bridge
processing/ range, scan, spatial, scene, reflection, freshness, timing
display/    DisplayPort, scope view, four pages, view cache, Chinese font
diagnostics/ DSP diagnostics
config/     contracts, geometry, generated profiles, hardware identity
```

Ownership rules:

- **Capture**: 128 SG descriptors, seven per frame, at most 18 preposted
  transport frames; software reclaims and refills, never enables cyclic
  overwrite.
- **RAW**: 16 × 512 KiB slots with monotonically increasing leases; a slot
  cannot be reused while owned; backlog ≥ 8 or data older than 500 ms triggers a
  controlled stop.
- **DSP**: two 3 MiB task slots, one PL engine; the PS prepares the next slot
  while post-processing the previous result.
- **Display**: immutable snapshots; a publication is complete only when the
  first real DPDMA acknowledgement matches the frame and data revision.

## 7. Time base

The PL exposes a 64-bit SYS tick snapshot with a capture epoch, plus a
64-record transport metadata queue (first/last beat ticks, byte count,
sequence). PS maps the tick to its own time with explicit uncertainty (100 ppm
allowance, 500 ms refresh, 1 s expiry) and refuses to report latency for
mappings worse than 1 ms.

Reported latency is **SYS first accepted FIFO→DMA handshake → first actual DP
acknowledgement**. It excludes ADC/RF time and is never presented as an
end-to-end physical latency.

## 8. Memory map

| Region | Start | Size | Purpose |
|---|---:|---:|---|
| Descriptor zone | `0x0020_0000` | 2 MiB | 128 SG descriptors (8 KiB used) |
| RX pool | `0x0040_0000` | 8 MiB | DMA target, 128 × 64 KiB |
| Capture store | `0x00C0_0000` | 8 MiB | 16 × 512 KiB RAW slots |
| Display frames | `0x0140_0000` | 16 MiB | two 8 MiB RGBA pages |
| Display control | `0x0240_0000` | 2 MiB | DPDMA descriptors |
| DSP workspace | `0x0260_0000` | 16 MiB | jobs, cube, RD, spectrum, candidates, temporal state, snapshots |

Ordinary application memory stays below 2 MiB.

## 9. Display

1920×1080p60 through the PS DisplayPort hard block, 148.5 MHz pixel clock,
2,200 × 1,125 totals, RGBA8888, two 8 MiB buffers. Pages are drawn into the
buffer that is not currently being scanned out; presentation is acknowledged by
DPDMA before the next data revision is accepted. Four pages: 系统总览,
方位热图, 平面回波监测 (with a ±60° reference field-of-view overlay), 空间分布.

## 10. Boot

The repository targets the F20's on-board 32 MiB QSPI (`is25wp256`). The boot
image is ordered FSBL → bitstream → application so that the FSBL's temporary DDR
usage cannot overwrite the application:

1. FSBL initialises DDR and the PS.
2. The bitstream configures the PL.
3. The A53 application runs admission (DMA qualify, memory checks), initialises
   DisplayPort once, starts RF, and only reports success after the first frame
   passes CRC, PL processing and display acknowledgement.

Measured cold boot: application autostart 12.675 s after reset (excluding
BootROM/FSBL), 979/978/977 acquired/processed/presented in the short run.

## 11. Verification model

Evidence is versioned and gated:

| Gate | What it proves |
|---|---|
| XSim regressions | RTL behaviour, cancellation, clock-stop and replay cases |
| Bit-exact DSP models | Native FFT HDL and engine versus an independent fixed-point model |
| Validate / synthesis / implementation | Legal design, timing closure, DRC, CDC review |
| Board sessions | Real acquisition, CRC/HSI integrity, display acknowledgement, long runs |

Internal timing closure is never reported as external input-timing sign-off, and
a successful simulation is never reported as board verification.
