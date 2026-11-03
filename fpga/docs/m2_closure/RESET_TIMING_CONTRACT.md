# PHY reset order and native timing proof

This is the current internal R01/R05/R06 contract, not board signoff. The earlier
common IDELAY/IDELAYCTRL reset and later ISERDES reset violated the required
component sequence; the new implementation corrects that internal defect.

## Ordered execution

`u_phy/u_bitclk_buf` remains active when external BITCLK exists and clocks the
full-rate release qualification. Two parallel BUFGCE_DIV cells share input,
CE and CLR: `u_serdes_div` divides by one and supplies ISERDES CLK/CLK_B;
`u_rx_div` divides by four and supplies every IDELAYE3 CLK and ISERDES CLKDIV.
CLK_B inversion is internal. There is no LUT-gated clock or dedicated-route override.

1. REF clears its registered `clock_enable_ref`. Both buffer CLR inputs assert
   through the independent full-BITCLK helper's asynchronous assertion path.
2. After three-stage synchronized `gate_open_ref=0`, REF waits eight additional
   cycles before asserting the six primitive resets and IDELAYCTRL reset.
3. Hold reset for 64 REF cycles; deassert all IDELAY and ISERDES primitive resets
   first, wait 16 REF cycles, then deassert IDELAYCTRL RST.
4. Require actual RDY plus another 16 REF cycles before clock_enable_ref rises.
   Both dividers stay cleared until the full-BITCLK helper completes its
   three-stage synchronization and four-cycle hold.
5. RX establishes a real baseline; application release still requires its
   subsequent 64 RX-cycle qualification. ARM admission can precede BITCLK;
   capture activation requires actual RX readiness.

Primitive/controller reset source registers have INIT=1 and change only at REF
edges. Runtime reset first closes the clocks, then REF performs the sequence;
it does not asynchronously assert these primitive RST outputs. Configuration/GSR
is a separate case. Current source-hashed XSim evidence must include reset and
RECAL, BITCLK stopped high and low, clock return, and ARM before BITCLK.

The ordering follows AMD [UG571 Component Mode Reset Sequence, printed pages
188–189](https://www.amd.com/content/dam/xilinx/support/documents/user_guides/ug571-ultrascale-selectio.pdf).
The related parallel-buffer hazard is described by [UG906 TIMING-49](https://docs.amd.com/r/2025.1-English/ug906-vivado-design-analysis/TIMING-49-Unsafe-Enable-or-Reset-Topology-from-Parallel-BUFGCE_DIV).

## Physical bounds and preserved checks

Only the exact REF primitive-reset registers to the six primitive RST pins
receive `set_max_delay 8.000`. It omits `-datapath_only`, which would suppress
hold checking and hide required removal evidence: [UG835 set_max_delay](https://docs.amd.com/r/2025.1-English/ug835-vivado-tcl-commands/set_max_delay).
No user false path touches these RSTs, either divider CLR, or the final local
release recovery/removal endpoints. Controller RST retains native same-REF
timing; it receives no relaxed max-delay exception.

`check_phy_reset.tcl` requires nine distinct current endpoints: six primitive
RSTs, two divider CLRs and controller RST. Every endpoint needs nonempty max and
min propagation paths, exact registered sources, physical datapath delay no
greater than 8 ns, and the effective exception. The actual 2025.1 library has
recovery/removal arcs for both divider CLR pins and IDELAYCTRL RST; their routed
slacks must be finite and nonnegative. Synthesized estimates are recorded as
`PRE_ROUTE`, including negative estimates, so placement/routing can fix them.
They cannot qualify release.

The mapped IDELAYE3 and ISERDESE3 libraries have no setup/hold/recovery/removal
arcs terminating at RST. Their minimum paths consequently have empty native
SLACK/REQUIREMENT properties (the text report displays an unconstrained/inf
result). The checker requires each exact primitive type, its complete native
arc inventory, zero such check arcs, existing non-user-disabled reset
propagation arcs, and the real minimum propagation path. It records
`NO_LIBRARY_CHECK` with JSON null slack/requirement and the empty raw values;
it never invents a finite minimum slack. The six 8 ns maximum bounds and the
stopped-clock reset protocol remain mandatory. A changed library model fails
for review. Both synthesis and route must provide current library evidence.
False-path/datapath-only reset exceptions fail. Each path receives a separate
report; aggregate WNS is insufficient.

The primitive-to-controller guard is `16*TREF`. An 8 ns primitive-reset physical
bound leaves `16*TREF - 8ns` before the earliest controller-release launch
(approximately 45.3 ns). Actual periods include Vivado's 1 ps quantization.

`gate_open_ref` observes the release register, not the physical buffer output.
The closing proof therefore reads each current buffer's `CLR -> O` `Reg Set/Clr`
library arc and its `DELAY_MAX_FALL`, requiring coverage of FAST/SLOW maximum
fall delays. Absent, non-finite, zero or user-disabled arcs cannot pass.

```text
closing_margin = 8*TREF
               - 8ns                         # release-FF -> CLR physical bound
               - max(current CLR->O delay)
               - max(current primitive clock-tree latency)
```

The clock propagation term is measured separately on all 12 primitive clock
loads (three IDELAY CLK and three ISERDES CLK/CLK_B/CLKDIV groups). Native
`get_net_delays -of_objects <one net> -to <pin>` returns FAST_MAX/SLOW_MAX in
picoseconds; the checker converts their maximum to ns. It does not use the
unclocked primitive RST endpoint's zero ENDPOINT_CLOCK_DELAY as a substitute.
All 12 objects must exist and final route must report ESTIMATED=0. The local
2025.1 API was exercised in `mapped_probe_02/net_delay_api03.log`; the command's
units and scope are documented in [UG835 get_net_delays](https://docs.amd.com/r/en-US/ug835-vivado-tcl-commands/get_net_delays).
No extra margin is credited from the three REF feedback stages. Both divider
arcs must exist and remaining margin must be positive. The API exposes native
[timing arcs](https://docs.amd.com/r/2024.1-English/ug835-vivado-tcl-commands/get_timing_arcs)
and [TIMING_PATH properties](https://docs.amd.com/r/2025.1-English/ug912-vivado-properties/TIMING_PATH).

An isolated old checkpoint was used only to discover the 2025.1 API/library
fields. Its /4 clear maximum was 0.213 ns; that number is not hardcoded into
production and does not qualify the new design. Diagnostic log:
`outputs/m2_closure_20261013_c1/reset_arc_api_old_01/native.log`.
Current synthesis/route must produce their own objects, library values and
reports, bound by the build and route receipts. State-controlled clock gating
may still need exact CDC review; broad asynchronous-clock exceptions are forbidden.
