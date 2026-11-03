# mmWave Radar on Zynq UltraScale+ (F20 / XCZU2CG)

A complete, open-source **60 GHz mmWave radar acquisition → PL signal processing → 1080p60 display** stack running on a Milianke **F20** board (`XCZU2CG-SFVC784-1-E`) with a TI **IWR6843ISK** radar head.

The design captures the radar's raw LVDS ADC stream (2 × 600 Mb/s, 300 MHz DDR bit clock), moves it into DDR through an AXI DMA scatter-gather engine, runs the radar arithmetic chain inside the programmable logic (range FFT → Doppler FFT → power → 2D CFAR → peak selection), renders a four-page Chinese instrument UI over DisplayPort, and can boot autonomously from on-board QSPI flash.

```
 IWR6843ISK             F20 (XCZU2CG)                                     1080p60 DP
┌──────────┐  LVDS   ┌──────────────────────────────────────────────┐   ┌──────────┐
│ 3TX / 4RX│ 2×600M  │ PL: IBUFDS → IDELAYE3 → ISERDESE3 → RAW16     │   │ 4 pages  │
│ 60 GHz   │────────►│     → 64-bit AXIS → async FIFO → AXI DMA SG   │   │ 中文界面 │
│ FMCW     │ BITCLK  │                                              │   │          │
│          │ 300M    │ PL: radar_dsp_engine (range/Doppler FFT,      │   │          │
│          │ FRCLK   │     power, CFAR, top-32) + AXI-Lite CSR       │   │          │
└──────────┘         │                                              │   │          │
                     │ PS: Cortex-A53 bare metal                    │   │          │
                     │     capture session / SM / scene / display ──┼──►│          │
                     └──────────────────────────────────────────────┘   └──────────┘
```

## Status

| Item | Value |
|---|---|
| Latest release | **v1.12.0** / build `20261103_autoboot_01` |
| Hardware | XCZU2CG-SFVC784-1-E, Milianke F20 + IWR6843ISK |
| Tools | Vivado 2025.1, Vitis 2025.1, bare-metal AArch64 |
| Timing (routed) | WNS **+0.627 ns**, WHS **+0.011 ns**, 0 failing endpoints, 0 routing errors |
| Resources | LUT 33,114 (70.1%), FF 33,310 (35.3%), BRAM 78 (52%), DSP 56 (23.3%) |
| Streaming | **9.997 frames/s**, 6213 frames / 621 s, interval P95 100.1 ms |
| Display latency | P95 **149.8 ms** (SYS first accepted beat → first real DP acknowledgement) |
| Boot | QSPI `BOOT.BIN` (6,280,976 B), verified cold boot without JTAG |
| License | MIT (see `LICENSE`, `NOTICE.md`) |

> This repository is a curated source release. Build outputs, bitstreams, XSAs, ELFs, archived baselines and multi-gigabyte measurement logs are **not** included; every number above is reproducible from the sources plus the documents under `software/verification/`.

## Repository layout

| Path | Contents |
|---|---|
| `fpga/rtl/` | Production RTL: LVDS PHY, RAW16 decoder, packetizer, control plane, clock-domain mailboxes, DSP engine (21 files) |
| `fpga/constraints/` | Clock, CDC and pin constraints (`radar_clocks.xdc`, `radar_pins.xdc`, `radar_cdc.xdc`) |
| `fpga/sim/` | SystemVerilog testbenches, including the DSP regression suite (`sim/pl_dsp/`) |
| `fpga/scripts/` | Build/closure/verification Tcl and Python entry points |
| `fpga/Radar_XCZU2CG_M2.srcs/` | Block design `radar_system.bd` and BD-level IP configuration |
| `fpga/ip/pl_dsp/` | IP **configuration only** (`.xci`); output products are regenerated locally |
| `software/radar_runtime/src/` | Bare-metal application: app, HAL, capture, processing, display, config, linker script |
| `software/tools/` | Host-side generators, decoders, comparators and build checks |
| `software/tests/` | Host unit tests, mocks and vendor-model shims |
| `software/docs/` | Runtime documentation, including `CURRENT_RUNTIME.md` |
| `software/verification/` | Per-milestone delivery records (`RESULT.md`, contracts, audit notes) |
| `software/third_party/ui_font/` | Noto Sans CJK SC licence, provenance and subset manifest |
| `docs/ARCHITECTURE.md` | Consolidated architecture description of the current release |
| `archive/` | Placeholder for locally kept baselines (not published) |

## How it works

1. **Capture.** Four LVDS pairs (BITCLK, FRCLK, DATA0, DATA1) enter Bank65. Each lane goes through `IBUFDS → IDELAYE3 → ISERDESE3` (1:8, 300 MHz / 75 MHz), the FRCLK 16-bit pattern locks the word boundary, and two lanes are packed into a 64-bit AXI4-Stream. A 8192×64 asynchronous FIFO crosses into the 100 MHz domain and an AXI DMA (S2MM, scatter-gather, 128 descriptors) writes 64 KiB slices into DDR. One radar frame is exactly 399,360 bytes = 96 chirps × (64 B HSI + 4096 B ADC), i.e. seven descriptors.
2. **PL arithmetic.** `radar_dsp_engine` reads the packed I/Q from DDR, applies DC removal and a Hann window, runs a 256-point range FFT (AMD XFFT, Radix-2 Burst, 24-bit), transposes tiles in DDR, runs a 32-point Doppler FFT, merges 12 channels, performs 2D CFAR with 3×3 non-maximum suppression and emits the strongest 32 candidates. A full job takes 5,535,727 cycles ≈ 55.36 ms at 99.999 MHz.
3. **PS software.** A bare-metal Cortex-A53 application owns capture sessions, descriptor rings, RAW-slot leases, calibration/TDM compensation, angle estimation, the scene/reflection logic and the four-page display. Only the PS prepares jobs and consumes results; it never bit-bangs the radar interface.
4. **Streaming.** `STREAM` mode keeps 16 × 512 KiB RAW slots and two 3 MiB DSP task slots in flight, so preparation, PL execution and display acknowledgement overlap. Backlog or stale data triggers a controlled stop instead of silent drops.
5. **Display.** 1920×1080p60 over the PS DisplayPort with two 8 MiB buffers and descriptor-acknowledged presentation. Pages: 系统总览 / 方位热图 / 平面回波监测 / 空间分布.
6. **Boot.** FSBL → bitstream → application, packed as a QSPI `BOOT.BIN`; the application runs the full admission sequence, initialises DisplayPort once and starts RF by itself.

See `docs/ARCHITECTURE.md` for clock domains, the memory map and the register-level contracts.

## Building

### FPGA

```text
Vivado 2025.1 → open fpga/Radar_XCZU2CG_M2.xpr
1. Report IP Status → Generate Output Products (the two XFFT cores and the DataMover
   are shipped as .xci only; regenerate them, or run scripts/m2_pl_dsp/create_fft_ips.tcl)
2. radar_system.bd → Generate Output Products → Create HDL Wrapper
3. Run Synthesis → Implementation → Generate Bitstream
4. File → Export → Export Hardware (include bitstream) to produce the XSA
```

Constraints assume the default `C:/Xilinx/2025.1` installation; adjust the paths in `fpga/scripts/` if your tools live elsewhere.

### Software

```text
Vitis 2025.1 → create a platform from the exported XSA (AArch64, standalone BSP)
            → create an application from software/radar_runtime/src
              (CMakeLists.txt is included; linker script src/linker/lscript_sw4.ld)
            → build radar_runtime.elf
```

`software/docs/INSTALL_AND_ACCEPTANCE.md` describes the expected memory layout and acceptance checks. The application refuses to run if the XSA/bitstream identity embedded in `src/config/radar_dsp_hw_identity.h` does not match the hardware it finds.

### Persistent boot (optional)

```text
bootgen -image boot.bif -arch zynqmp -o BOOT.BIN -w on
```

with partitions ordered FSBL → bitstream → application ELF, then program QSPI (Vitis "Program Flash" or Uniflash). `software/verification/20261103_autoboot_01/boot_image/BOOT_OPERATION.md` documents the exact sequence and the recovery procedure.

## Measured results

| Milestone | Result |
|---|---|
| Raw capture (v1.7) | First frame 399,360 B, CRC32 `c22be9ae`, 96 HSI records, 384 range FFTs |
| Numerics | Board vs NumPy max deviation 1.01×10⁻³ dB; 196,608 I/Q components bit-identical |
| PL DSP (v1.9) | RMS 9.16×10⁻⁶, spectral error 0.00149 dB (gates 1e-5 / 0.1 dB) |
| Streaming (v1.10.1) | 6213 frames, 621.2 s, 9.9967 Hz, interval P95 100.1 ms, 0 CRC/HSI/DMA/metadata errors |
| Long run (v1.11.1) | 6211 frames, 621.0 s, 9.9992 Hz, latency P95 134.5 ms, scene CPU P95 1.72 ms |
| QSPI boot (v1.12.0) | Cold boot with JTAG unplugged, application autostart 12.675 s, 979/978/977 in the short run |
| Key navigation | KEY3 short press → next page, hold ≥ 1 s → overview; verified on hardware |

## Key navigation

KEY3 (`PS_BUT`, MIO43, active-low, external 2.2 kΩ pull-up to 1.8 V) is polled
every 5 ms with a 30 ms debounce. A short press advances to the next page; holding
the button for one second returns to the overview. The path only requests a page
from the display scheduler — it never touches capture, DMA or the radar UART.

The button is implemented and has been verified on hardware. Some per-milestone
`RESULT.md` files state that KEY3 was *not tested in that round*; those lines
describe the scope of that specific round (the page walks were driven through the
software page interface instead), not the state of the feature.

## Known limitations

These are deliberate, documented boundaries — please read them before citing the project:

- **The weak-target enhancement did not meet its gate.** On a frozen independent set it missed 309/309 weak-target frames (0% improvement against a ≥20% requirement). `PL_ENHANCED` is experimental; the PL arithmetic itself is verified.
- **The full 80 ms/frame CPU budget is not proven.** Instrumented kernels reach P95 ≈ 61 ms, but main-loop, UART and console overhead are not fully accounted.
- **Spatial output is uncalibrated.** Angles/height come from a nominal array (`UNCALIBRATED / NOMINAL ARRAY`); `HUMAN?` and `WALL?` are experimental rule outputs, not classification accuracy.
- **External LVDS input timing is still pending.** The `set_input_delay` template is not imported; internal timing closure does not imply a signed-off input sampling window.
- A DisplayPort startup flag (`0x8`) latched during initialisation is retained; audits only state that no *new* DP errors were observed in scope.
- Booting was verified once with a physical power cycle, not over repeated power cycles or long-duration soak.

## License

MIT — see [`LICENSE`](LICENSE). Third-party components and tool-generated IP are described in [`NOTICE.md`](NOTICE.md).

## Acknowledgements

Milianke F20 documentation, TI mmWave SDK and the AMD Vivado/Vitis toolchain made this work possible. Thanks to everyone who reviewed the CDC, reset and timing contracts.
