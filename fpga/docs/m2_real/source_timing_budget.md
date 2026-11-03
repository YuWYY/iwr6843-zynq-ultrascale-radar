# M2 source timing and CDC budget

Status: **R01-R07 closure in progress; external source timing PENDING**.
This separates the internal handshake bound from the unknown external timing.
Neither RTL simulation nor static implementation establishes the physical
sampling margin of the radar input.

R07 public-source review (2026-10-13): see
`docs/m2_closure/R07_SOURCE_TIMING.md` for TI SWRS219F Rev F (2025-04),
sections 7.12.4/7.12.4.1 and figures 7-12/7-13, and SWRA581B section 6.
The supported 600 Mb/s / 300 MHz DDR mode is established by the TI datasheet;
the actual flashed firmware/configuration is not established. Its 900 Mb/s
typical jitter/rise-time entries are not a 600 Mb/s worst-case input-delay
contract. Board-path skew and the actual per-edge source min/max remain unknown.
Sources: [TI IWR6843 datasheet](https://www.ti.com/lit/ds/symlink/iwr6843.pdf),
[TI ADC raw-data capture note](https://www.ti.com/lit/an/swra581b/swra581b.pdf).

## Clocks and constraint application

The supported input is 600 Mb/s per lane, with a 300 MHz forwarded DDR BITCLK
(3.333333 ns period, 1.666667 ns UI). `u_phy/u_rx_div` is a BUFGCE_DIV configured
for divide-by-four, producing 75 MHz RX (13.333332 ns period). System clock is
100 MHz (10 ns); IDELAY reference is 300 MHz from the existing Clocking Wizard.
The PLL-derived reference clock is not phase-related to radar BITCLK.
Vivado represents clock times in 1 ps steps: the requested BITCLK period is
realized as 3.333 ns and its /4 clock as 13.332 ns. The native checker compares
RX against four times the realized BITCLK period. Values in the budget table
are nominal; this picosecond quantization is included in the approximately 2 ns
fastest-channel reserve.

All three imported XDC files are pure declarations: the Vivado 2025.1 XDC reader
does not execute `foreach`, `if` or `puts` bodies. The native Clocking Wizard
board XDC owns the 100 MHz reference input clock; `radar_clocks.xdc` declares
the BITCLK primary and generated clocks at the actual BUFGCE_DIV outputs:
RX divide-by-four and ISERDES divide-by-one, both from BITCLK and without `-add`.
Their physical buffers share input, enable and clear. Wizard outputs remain derived.
Object, waveform, period and master-clock assertions run in full Tcl through
`check_constraints.tcl` after synthesis. An absent clock or divider is a hard
audit failure; an RTL-stage view is not a substitute for that netlist check.
No independent 75 MHz clock, duplicate added clock, global clock-group false
path or dedicated-clock-route override is used.

`radar_pins.xdc` and `radar_clocks.xdc` are normal top-level constraints.
`radar_cdc.xdc` is top-level, LATE and **USED_IN_SYNTHESIS=false** because it
requires final primitive register names. It applies during implementation;
after opening the synthesized design, explicitly `read_xdc` this file before
the audit if it has not already been loaded. Do not source it against an
elaborated RTL design. `scripts/m2_closure/check_constraints.tcl` inspects an open
netlist without saving the project or adding exceptions.

## Held-payload handshake bound

The reviewed transfers use a held payload and three synchronizer registers. At
launch edge L the source captures the payload and toggles the request (mailbox)
or acknowledgement (snapshot). Let E0 be the first destination edge able to
observe the toggle. Nonblocking assignments produce this sequence:

| Destination edge | New synchronizer state / capture action |
|---|---|
| E0 | stage 0 sees the toggle |
| E1 | stage 1 sees the toggle |
| E2 | stage 2 sees the toggle; capture logic still sees the old stage 2 |
| E3 | capture logic sees changed stage 2 and captures the held payload |

Thus the earliest nominal launch-to-capture interval is `delta + 3*Tdst`, where
`0 <= delta < Tdst`; metastability may postpone recognition and is not assumed
to improve the minimum bound. Source payload must remain unchanged until the
acknowledgement protocol permits another transfer. Reset/abort invalidates an
in-flight transfer; it is not an additional useful data transfer.

| Instance | Direction | Minimum nominal interval | Applied max delay / bus skew |
|---|---|---:|---:|
| `u_cmd_mb` | SYS100 -> RX75 | 40 ns | 8 ns / 8 ns |
| `u_phy/u_delay_mb` | SYS100 -> RX75 | 40 ns | 8 ns / 8 ns |
| `u_phy/u_delay_report_mb` | RX75 -> SYS100 | 30 ns | 8 ns / 8 ns |
| `u_phy/u_recal_mb` | SYS100 -> REF300 | 10 ns | constant request token: handshake only if no live payload |
| `u_phy/u_cal_report_mb` | REF300 -> SYS100 | 30 ns | 8 ns max delay; inter-bit skew is not applicable to one bit |
| `u_snapshot` | RX75 -> SYS100 | 30 ns | 8 ns / 8 ns |
| `u_tap_snapshot` | RX75 -> SYS100 | 30 ns | 8 ns / 8 ns; 64-bit format, bits 0..26 and 32..63 carry measurement/count |

The original common 10 ns maximum equalled the fastest nominal interval. The
common bound stays at 8 ns, preserving the original conservative 2 ns reserve
even for a future reviewed REF300 payload. In the present topology the REF300
request is event-only: live payloads terminate in SYS100 or RX75, so their
minimum nominal capture windows are 30 ns or 40 ns, respectively. No live
payload currently relies on the smaller 10 ns REF300 window.
It is applied between sequential source/destination cell objects using
`set_max_delay -datapath_only`, plus `set_bus_skew` for multi-bit buses. The
dynamic one-bit calibration result still requires its actual max-delay path;
the checker accepts no multi-bit channel under that scalar-only skew exclusion.
Vivado includes launch
clock-to-Q, interconnect/logic delay and destination setup, while excluding clock
skew/jitter from this maximum calculation. The command intentionally suppresses
the unrelated inter-clock hold check; the bounded maximum path remains timed.
The nominal reserve is not a measured guarantee: inspect implemented timing,
setup requirements, clock arrival differences/uncertainty and bus-skew reports.
If those effects consume the reserve, tighten the bound or revise the reviewed
handshake; do not waive the path. A clock-frequency or synchronizer-depth change
requires this budget to be recalculated.

The new netlist contract explicitly lists five mailbox parents and two snapshot
parents. It checks the reviewed source/destination bit sets and every surviving
destination D, including physical replicas. Constant/unused bit elimination
requires an explicit bit-list rationale; a nonzero count alone cannot pass.
The old 585 endpoint total is historical and is not a target for the new design.
See `scripts/m2_closure/cdc_contract.tcl` and `docs/m2_closure/clock_channels.md`.
An event-only constant token is still checked for its request/acknowledgement
structure, zero destination payload registers and zero functional consumers of
any retained source payload register; it must not receive an empty max-delay
declaration. The initial REVIEWED contract freezes source-level topology and
explicit KEEP-register expectations; it is not a claim that native synthesis
has already passed. Native bit-population mismatches fail and require reviewed
contract changes plus a fresh build identity and synthesis.

Only first-stage one-bit synchronizer D pins receive data false paths.
Reset-release now uses `xpm_cdc_async_rst` and its vendor-scoped reset exception;
raw asynchronous reset is not excepted onto counters or released outputs.
Counter reset is synchronous; the released-register assertion comes from the
local XPM last stage, whose recovery/removal paths remain timed. Subsequent
one-bit synchronizer stages remain timed. Three unclocked low-speed output ports
retain their narrow exceptions. The former standalone delay-completion toggle
is replaced by explicit request/result mailbox ownership and is not reused as
an unqualified completion proof. All new channels are in the new contract.

The ordered component reset and dedicated-clock gating proof are specified in
`docs/m2_closure/RESET_TIMING_CONTRACT.md`. Primitive RSTs have a narrow 8 ns
max-delay bound without `-datapath_only`; divider CLR and final local release
recovery/removal paths remain timed. Current library clear delay and actual
clock-tree propagation are included in the REF quiesce guard.

## External source-synchronous delay — missing inputs

For each of FRCLK, DATA0 and DATA1 and each launch edge, a reviewed interval is
needed at the FPGA pins relative to the forwarded BITCLK edge:

`input_min = source_min + (data_path_min - clock_path_max) - early_uncertainty`

`input_max = source_max + (data_path_max - clock_path_min) + late_uncertainty`

The selected firmware clock/data alignment determines the reference edge and
source interval. Include applicable source timing variation and relative
clock/data jitter exactly once; if represented separately as clock uncertainty,
do not also include it in these delay numbers. Board/adapter propagation must
include the actual complete connection. Common-mode clock jitter and relative
data-clock jitter are different quantities. Do not assume zero external delay,
zero board skew, centre-aligned output, or that default IDELAY=0 centres the eye.

The optional `radar_source_timing_template.xdc` takes these four reviewed values
in ns: `RADAR_IN_MIN_R_NS`, `RADAR_IN_MAX_R_NS`, `RADAR_IN_MIN_F_NS`,
`RADAR_IN_MAX_F_NS`. Rising and falling entries are both required. Its common
interval must enclose the independently reviewed intervals of all three lanes;
use separate reviewed per-lane delays if that conservative common interval
cannot close timing. Negative differential pins are not separate data streams.
Before importing the pure-declarative template, the full Tcl procedure
`m2_validate_source_timing_values` refuses missing, non-finite or reversed values.

PENDING: actual radar board/firmware/configuration; output alignment and source
min/max timing across operating conditions; actual lane/clock propagation and
skew; uncertainty allocation; physical voltage/termination; idle-clock preamble
and verified FRCLK format. See `board_io_contract.md` and `RAW16_CONTRACT.md`.
The template stays outside automatic constraint import until these are reviewed.
Internal timing may be assessed provisionally, but external capture timing and
board operation must remain explicitly unsigned off.
