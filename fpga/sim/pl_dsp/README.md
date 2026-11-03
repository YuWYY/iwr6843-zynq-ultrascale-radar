# Offline DSP verification

The production datapath is in `rtl/dsp`. These files are test sources and must
not be added to production synthesis. No command below contacts a board or a
serial port. Use a new output directory for each run; failures are retained.

## Evidence boundaries

| Layer | What runs | What it proves |
|---|---|---|
| Native FFT probe | Generated AMD FFT HDL, impulse/constant/positive complex tone | Forward sign, natural order, scaling, TLAST, status |
| Native CFAR | Production detector RTL | Training windows, ties, top 32, fresh gate, raw output units, 72-bit sum without wrap |
| Native engine | Production engine, official XPM, generated AMD FFT HDL | Complete implemented datapath, native AXIS behavior and simulation-cycle count under the stated DDR peer |
| Fast co-simulation | Same engine/XPM with AMD FFT C-model called through DPI | Exact fixed-point arithmetic, DDR transpose, accumulation, temporal state, detection and controller regressions |
| Batch co-simulation | 64 uninterrupted jobs, 8/16 reusable input slots | Sequential start/DONE/reuse, identities, gaps, epoch reset and exact output/state comparisons |

`fft_vendor_axis.sv` is a **simulation-only adapter**, not the AMD HDL model.
It calls the generated official DLL through `fft_vendor_dpi.c`; it contains no
FFT implementation. Its configured waits are 1024 clocks after the last Range
input and 256 after the last Doppler input. Its cycle count cannot establish
real FFT latency, physical DDR bandwidth, or hardware frame rate.

The AMD interface is documented in
[PG109](https://docs.amd.com/r/en-US/pg109-xfft/FFT-C-Model-Interface), and the
Windows DPI build is supported by
[UG900](https://docs.amd.com/r/2025.1-English/ug900-vivado-logic-simulation/xsc-Compiler).

## Repeatable entry points

From this directory, with Python and NumPy available:

```powershell
python ./run_verification.py --mode cosim --output /tmp/new_dsp_check `
  --temporal-model /software/verification/20261101_pl_dsp_01/scripts
```

Use `--mode native` for the original generated FFT HDL, or `--mode all` for both.
The native full-frame models can take hours; neither the DPI result nor the
official C-model alone substitutes for a pending native result. The runner
fails on tool errors, missing PASS markers, numerical disagreement or source
changes, and writes `summary.json` with source hashes and retained logs.

The native units can be repeated independently:

```powershell
vivado -mode batch -source ./run_native_unit.tcl -tclargs probe /tmp/new_fft_probe
vivado -mode batch -source ./run_native_unit.tcl -tclargs cfar /tmp/new_cfar_probe
```

For the 64-job test, supply the `fixture` and `vendor_cmodel` folders generated
by `run_verification.py`:

```powershell
./run_batch_verification.ps1 -Output /tmp/new_batch_check `
  -Fixture /tmp/new_dsp_check/fixture -Vendor /tmp/new_dsp_check/vendor_cmodel `
  -Models /software/verification/20261101_pl_dsp_01/scripts
```

The independent numerical and temporal reference modules are passed explicitly
through `-Models`; their source and the actual archived RAW/config must remain
available. The 64th job replays that archived physical RAW after a fresh epoch.
This is archived data replay, not a new physical capture or a contiguous
physical sequence. Other jobs use twelve amplitude/phase variants. Injected
100 ms intervals and the 800 ms gap are test semantics.

The batch uses a **test-only** 16 MiB layout: up to 16 input slots beginning at
`0x02600000` with `0xC0000` stride; Range at `0x03200000`, cube at `0x032C0000`,
RD at `0x03380000`, spectrum at `0x03390000`, candidates at `0x03391000`, temporal
state at `0x03400000`. It does not change the production PS memory contract.
Each previous job must have reached DONE with all DDR transactions drained
before its input slot is reused. Actual outputs are compared word by word;
compact per-job hashes/counts are retained, and a failing region/input is saved.

## Production arithmetic and storage

The mean and Hann products use round-to-nearest-even. Raw signed16 input is
shifted left by 7; components remain signed24 in separate signed32 DDR slots.
Both native FFTs use the approved **Radix-2 Burst I/O**, `C_ARCH=2`,
non-real-time AXIS backpressure, natural output order and convergent rounding.
The native generated config widths are 24 bits for Range and 16 for Doppler.
FWD_INV occupies bit0 and SCALE_SCH begins at bit1; the eight/five Radix-2
stages each shift right by one. Configs are `24'h00AAAB` and `16'h02AB`,
respectively, giving 1/256 and 1/32. Native impulse/constant/positive-tone
probes verify these values. The earlier streaming-core evidence is archived as
`FAILED_STREAMING_DEVIATION` and is not acceptance evidence for this revision. Physical TX order is mapped from slots `[0,2,1]`.
Two alternating 16-range tile banks are serialized; this revision does not
claim concurrent tile prefetch. Explicit XPM SDP memories prevent the large
arrays from silently becoming LUTRAM.

Cases 1/2 cancel an accepted read/write, 3/4 inject read/write status faults,
5 rejects an invalid region, 6 injects early read TLAST, 7 exercises missing
trusted time, 8 exercises initialized temporal enhancement, and 9 enters an
explicit post-RD fault checkpoint to test real temporal overflow rejection.
Case 9 is controller/arithmetic fault injection, not a full FFT-frame proof.

Memory errors soft-halt both DataMover directions and set error bit31. A
quiescent local RECOVER is required before another job. Temporal score overflow
returns `0x303`, increments the overflow count, clears candidate validity and
leaves the external temporal header invalid. Any fatal write or cancellation
requires the PS recovery policy to reinitialize temporal state before reuse.

Synthesis, routing, timing, physical DDR performance and board behavior remain
separate acceptance gates.
