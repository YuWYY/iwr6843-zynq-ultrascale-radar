# v1.12.0 persistent startup work

User selected: power-on automatic continuous radar acquisition and TOP VIEW.
Final medium: fitted QSPI Flash on the user's confirmed CM02-2CG package C.
SD was considered earlier; it is not the selected implementation.

## Preserved baseline

- Successful v1.11.5 PS source/platform/ELF evidence: `/archive/20261103_autoboot_01/v1.11.5/` (2175 files, copy hashes verified).
- FPGA sources, XPR, IP, constraints, scripts and current synthesis/implementation products: sibling `fpga_before/` (274 files, copy hashes verified).
- No ZIP is produced. Radar algorithms, waveform, UI layout and KEY3 logic are retained.

## Current preparation

1. New cooperative PS startup FSM implemented and host/UBSan tested; independent review issues fixed. Final sources frozen in `checks/ps_integration/source_manifest.json`.
2. Matching native platform, QSPI FSBL and v1.12.0 application rebuild passed: `checks/platform_native/matched_qspi_01/result.json`. Earlier old-platform app build is historical only.
3. Current old XSA/FSBL had QSPI/SD disabled; media boot would be unsupported. `configure_qspi.tcl` uses native Vivado configuration, enabling only QSPI MIO0..5, single x4, 1.8V, no feedback clock, 200MHz reference (actual199.998001MHz). Existing PS clocks, DDR, radar/AXI nets and assigned addresses remain unchanged. The new native unassigned QSPI slave aperture is recorded.
4. Native configuration, BD Validate/generation, synthesis, routing, final checks and export all passed. WNS0.627ns/WHS0.011ns. Native evidence: `checks/qspi_native_stack.json`. All existing non-QSPI clock values and assigned addresses are retained.
5. BOOT image `boot_image/production_02/BOOT.BIN` passed native Bootgen and independent format/payload checks (6,280,976 bytes). The read-only helper built against the real QSPI BSP and its identity was sealed/rechecked. A controlled full Flash backup began at 13:27, archive `flash_before/attempt_20261103T052717_023405Z`; success is not yet asserted.

## Hardware status so far

No Flash has been erased or written. No new application has been downloaded.
COMx is identified by VID1A86/PID55D4/serial5867024710. Initial opens, including the first cold-start attempt, failed with Windows device error31. The targeted pnputil restart was denied and did not repair the device. No serial commands were transmitted by these failed attempts. After the user specifically reconnected F20 J2 (PS UART), Windows recorded a fresh arrival at13:03:26 and the sole passive recorder opened successfully: `board_sessions/cold_02`, Python PID33988. The user confirmed an actual cold start and SW1 allON without Run. JTAG at1MHz enumerates the expected C306A13AABCD cable and 14711093/5ba00477 devices; read-only BOOT_MODE is0x00000000. See `hardware_access/`. No new application has run and no RF-start command has been sent.

Remaining gates: repeatable whole-Flash backup; native programming and full readback; actual QSPI power-on handoff without debugger initialization; real automatic acquisition/DP and user view confirmation. No KEY3 testing.

## 13:54 backup/export progress

- Original helper completed two32MiB physical reads, full byte comparison and CRC in194.51s. JEDEC9d7019; both CRCb5691917. CPU was halted before export.
- First complete raw export is32MiB, host CRC/counts match the board and SHA256 is `d13bd2c1ab894a9cac2f07d7a6c70c678d7ddb252735797db43c856cb3764458`. The second export is still running. Do not accept the double-file backup yet.
- Actual1MHz JTAG throughput is approximately70s per2MiB. The original2400s transport deadline may expire near14:07 with the final chunks outstanding. Preserve that failure and use reviewed `flash_backup/export_resume/` only after the original controller exits, checking suspended CPU, exact park code and unchanged mailbox. It exports existing DDR only; no reset, download or new Flash read.
- New `flash_verify/` has a separately sealed post-program helper with two fresh full reads/byte comparison and two full SHA256 calculations. It only exports320B and compares the complete expected Flash, avoiding another64MiB raw transfer. The original backup is still fully exported twice. Native build, known vectors,32MiB host stream and actual first Flash export SHA tests passed; hardware verification remains pending.
- Sole serial owner is still cold_02; no new runtime app or RF command has run. Root owns all hardware.

## 14:13 programming in progress

- Original backup is now FULL_FLASH_READBACK_VERIFIED in `flash_before/export_resume_20261103T060920_478439Z/backup_verification.json`. Both32MiB files SHA d13bd2c1ab894a9cac2f07d7a6c70c678d7ddb252735797db43c856cb3764458, CRC b5691917. First40min export timeout and initial export-only port-closed failure retained; owned PC hw_server restart restored transport. Resumed only last4MiB from original halted-helper DDR; mailbox/park code unchanged, no new flash read/reset.
- Payload `boot_image/flash_payload_01/` passed. Exactly6291456bytes [0,0x600000), including10480 original tail bytes. Expected complete32MiB Flash SHA 2ca1eb6f0f2eb9294c6576f386d34e6445b07a097a8ade32f266599daf125c05.
- Native `flash_programming/program_01` started14:12:27. Detected is25wp256,32MiB,64KiB erase. Tool erased exactly0x600000 bytes and is blank-checking. Retain generic Xicom50-353 mismatch warning (selected qspi-x4-single vs detected is25wp256); actual JEDEC/type/capacity match expected physical device. Native completion/readback still pending.
- No independent QSPI boot or radar runtime has yet run. Do not power-cycle while programming.

## Completed physical QSPI boot

PASS_PERSISTENT_QSPI_AUTOBOOT. User confirmed actual cold power-on, SW1 ON/OFF/ON, JTAG removed, no Vitis Run, and automatic TOP VIEW with continuously updating data. See status.json and RESULT.md. Console closed after three read-only status queries; runtime left LIVE.
