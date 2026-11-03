# M2 current state — R07, 2026-10-14

Mainline: /fpga/Radar_XCZU2CG_M2.xpr
Vivado 2025.1 / xczu2cg-sfvc784-1-e / radar_m2_top; ABI=0x52430101.
Current fresh build: 20261014_r07_b05. Internal PHY/CDC/DRC/native-log gates PASS.
WNS +1.085 ns; WHS +0.010 ns; WPWS 0.000 ns.
Route 16932/16932, errors 0.
Synth DCP SHA256: f101dab18289ad9149a96c0c324cbfdf60bc7f8c33b89f93d7456f1a80b4339d
Routed DCP SHA256: 3a44b38b4bafaecec62c1d1aa3ba71237c3e5fcbfa8ab2b32213e65629483c15

183 script tests PASS: 140 test_gates_b04 + 43 test_source_timing_b02;
recipe hashes rechecked. Reused 17 actual XSim PASS with 31 input and 42 log
hashes verified; no RTL simulation rerun. SIM_PASS SHA256: 5e7a4b85b24337a6274cad33b2059afe0c4b668e3931fb4836a07eef9fb837d3.

Production RTL14, existing XDC and UI remain byte-identical. Parent BD has four
native AXI-Lite metadata additions; three XCI files only change corresponding
value_src metadata. The 2053 protected official values, topology and four maps
match the baseline. Native full target reset, parent validation/save and forced
regeneration repaired stale SmartConnect dependencies. Generated EXT=0/AUX=0,
ext tied high, aux tied to aresetn and actual compiled sources are checked.

User override: complete verification; hardware export is performed by the user.
HARDWARE_EXPORT=NOT_RUN_USER_MANAGED; BITGEN=NOT_RUN. No native dev-hw/candidate
stage, XSA staging or bitgen was executed for this build. Python-only candidate
preflight was rejected by the pending R07 evidence gate without artifacts.
Current mainline read-only native reopen PASS; no post-seal force validation
or regeneration used as new build evidence. Direct mainline/verification-files
delivery; no compressed project deliverable. Reports, manifest and RESULT are
in delivery/20261014_r07_01. Vitis build and board test were not run.

SOURCE_TIMING=PENDING; BOARD_ELECTRICAL=PENDING; DOWNLOAD_CANDIDATE=NO;
BOARD_TEST=NOT_RUN. Current internal WNS does not establish input sampling.
No final source-timing XDC or current bit exists. The only missing-facts table
is docs/m2_closure/R07_SOURCE_TIMING.md. The first possible qualified IDELAY
scope is RESET_CALIBRATED_INITIAL_ONLY, with runtime tap changes excluded.

b01 export failure, b02 superseded build, b03 generated-reset rejection and
b04 stale-module synthesis failure remain in /archive/20261014_r07_01.
They are historical evidence, not the identity of this build. Existing Oct13
R01-R06 simulation/diagnostics remain historical. No Flash, sensorStart, radar
reset, SG/DDR board proof or runtime application work is implied.
