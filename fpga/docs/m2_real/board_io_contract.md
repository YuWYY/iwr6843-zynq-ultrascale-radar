# M2 board I/O contract — physical confirmation PENDING

Updated 2026-10-14 for R07. `BOARD_ELECTRICAL=PENDING`; the configured design
and the actual board are separate evidence. Fixed balls and existing electrical
settings are preserved. The single list of information needed from the user is
in [R07_SOURCE_TIMING.md](../m2_closure/R07_SOURCE_TIMING.md).

| Signal | P / N ball, or single ball | Bank | Preserved design setting |
|---|---|---|---|
| Independent PL reference 100 MHz | AE5 / AF5 | 64 | DIFF_SSTL12; board AC coupling, bias and external termination |
| Radar BITCLK | L7 / L6 | 65 | LVDS; DIFF_TERM=TRUE |
| Radar FRCLK | L3 / L2 | 65 | LVDS; DIFF_TERM=TRUE |
| Radar DATA0 | K8 / K7 | 65 | LVDS; DIFF_TERM=TRUE |
| Radar DATA1 | J6 / H6 | 65 | LVDS; DIFF_TERM=TRUE |
| UART TX / RX | AG10 / AH10 | 44 | LVCMOS33 |
| nERR / 3V3-present | AE12 / AF12 | 44 | LVCMOS33 |
| SYNC / nRST | AE10 / AF10 | 44 | LVCMOS33 |

UART TX, SYNC and nRST retain DRIVE=4 and SLEW=SLOW. 3V3-present retains a
pull-down. No FPGA pull-up is added toward an unpowered radar. UART directions
are from the FPGA viewpoint; adapter RS232 names alone do not establish the
radar-side direction. A PMIC_PGOOD net name is not a measured 3.3 V contract.

## Bank65: required supply and design evidence

**The target physical Bank65 VCCO is 1.8 V.** AMD UG571 v1.16, printed page 121,
requires this supply for HP LVDS inputs with internal differential termination.
`DIFF_TERM=TRUE` can be represented in the implemented design as
`DIFF_TERM_ADV=TERM_100`. The effective top-level port attributes and connected
IBUFDS instances must be checked; native I/O readback is not a voltage measurement.
[AMD UG571](https://docs.amd.com/api/khub/documents/kFbaUC5HGcXyGNauhgU6Gw/content)

The 2026-10-14 native readback of the existing routed DCP found TERM_100 on all
eight Bank65 input ports, while the four IBUFDS cell objects showed the legacy
DIFF_TERM default 0. These are not two equivalent property scopes. UG912 places
DIFF_TERM_ADV on the top-level PORT and documents automatic migration of the
legacy property; UG974 similarly places non-logic I/O properties on ports.
The gate checks the final port TERM_100 and the IBUFDS connections. No redundant
RTL parameter change is made to match a cell default. This design observation
does not establish a measured board termination or a qualified bitstream.
[UG912 DIFF_TERM_ADV](https://docs.amd.com/r/2025.1-English/ug912-vivado-properties/DIFF_TERM_ADV),
[UG974 IBUFDS](https://docs.amd.com/r/2025.1-English/ug974-vivado-ultrascale-libraries/IBUFDS)

The supplied F20 manual, printed page 33, identifies Bank65/66 as adjustable
1.2/1.8 V, Bank24 as fixed 1.8 V and Bank44 as fixed 3.3 V. The 20240606
baseboard schematic, sheet 14, identifies the Bank65 selector as HDR1: pin 1
is VCC_1V8, pin 2 is VADJ_BK65 and pin 3 is FEP_1V2. It labels the design
default as 1V8. **This does not prove the fitted jumper position or voltage.**
First match the actual board revision and silk screen. Adjust jumpers only with
power removed. If needed, measure at an identified accessible VCCO test point
and ground; BGA balls are not proposed as probe points.

## Termination and reference clock boundaries

The supplied adapter netlist contains CN1, CN3 and U1 connectors, fourteen
signal nets and no resistor component. LVDS mappings are BITCLK CN1.33/35 to
U1.34/36, FRCLK CN1.29/31 to U1.28/30, DATA0 CN1.43/45 to U1.46/48 and DATA1
CN1.37/39 to U1.40/42. This establishes netlist connectivity only, not routed
lengths or as-built assembly.

The local IWR6843ISK Rev D candidate schematic, sheets 4, 7 and 10, shows the
LVDS device and connector paths. Sheet 7 J1 has 0-ohm series components R22/R24
(FRCLK), R30/R32 (BITCLK), R33/R35 (DATA1) and R39/R41 (DATA0). No parallel
100-ohm termination is shown on these selected paths in those sheets. The
sheet 4 “100 OHMS” note describes differential trace impedance, not a resistor.
Rev D has not been identified as the connected board; other revisions also
exist locally. This review cannot exclude termination on another carrier,
cable, adapter revision or modification. Confirm the complete radar-board →
connector/cable → adapter → F20 path before accepting internal termination;
an additional 100-ohm shunt must not remain in parallel without review.

The core schematic, sheet 8, independently identifies the Bank64 100 MHz
reference path: C8/C9 AC coupling, R30/R31/R35/R36 1-kohm bias network and R34
100-ohm external differential termination, with bias from VDDR_1V2. This
supports the preserved DIFF_SSTL12 reference design. R34 is **not** termination
on the Bank65 radar inputs. Do not add a DIFF_TERM override to the reference
port or treat its Wizard-derived 300 MHz as phase-aligned to BITCLK. PS MIO
LVCMOS18 neither sets Bank65 power nor establishes Bank44 power.

The baseboard sheet 14 “5mil等长” note is a routing target, not a measured
full-path data-to-clock delay bound. Actual board revision, connector
orientation, ground, P/N polarity, lane mapping, power-off compatibility and
whole-path termination remain unconfirmed. Synthesis, route and DRC do not
complete these physical checks.

## Source identity

`delivery/20261014_r07_01/evidence/board_sources.json` records original local
paths, SHA-256 identities, reviewed pages and bounded search scope. Page
renderings beside it support this review. Original paths are provenance, not
active Vivado dependencies. The machine-readable contract is
`scripts/m2_closure/R07_contract.json`; physical evidence stays pending until
evidence for this particular assembled board is supplied and reviewed.
