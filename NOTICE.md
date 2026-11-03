# Third-party notices

This repository contains original design sources plus references to, and
configuration for, third-party components. No vendor binaries are
redistributed.

## AMD / Xilinx tool-generated IP

The design instantiates AMD LogiCORE IP:

- **XFFT** (`radar_fft_range`, `radar_fft_doppler`) — Radix-2 Burst I/O, 24-bit data.
- **AXI DMA** and **AXI DataMover** — scatter-gather capture and DSP memory traffic.
- **AXI GPIO**, **AXI4-Stream FIFO**, **SmartConnect**, **Clock Wizard**, **Processor System Reset**.

Only the IP **configuration** files (`.xci`, `_ooc.xdc`) are stored under
`fpga/ip/pl_dsp/` and the block-design IP definitions. Output products,
simulation netlists, DCPs and bitstreams are generated locally by Vivado and
are therefore not part of this repository. Their use is governed by the AMD
End User License Agreement that ships with Vivado/Vitis.

## Fonts

`software/radar_runtime/src/display/radar_font_zh_data.h` contains a subset
glyph bitmap derived from **Noto Sans CJK SC Regular**, licensed under the
SIL Open Font License 1.1. The full licence text, provenance record and subset
manifest are kept in `software/third_party/ui_font/`. The font binary itself
is not redistributed; regenerate the header with `software/tools/generate_ui_font.py`.

## Texas Instruments

The radar configuration files (`radar_3tx_hsi_*.cfg`) are project data written
for the TI IWR6843ISK. No TI firmware, mmWave SDK binary or RF configuration
from TI is redistributed.

## Milianke F20

Board pin assignments and electrical notes refer to the public Milianke F20
documentation. Board schematics and manuals are not redistributed.
