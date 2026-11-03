# DP diagnostic observation scope

Latest completed scope: `board_checks/stream620_fixed_01/dp_scope_observed_audit_01.json`, 6213 frames, status `NO_NEW_DP_ERROR_OBSERVED_IN_SCOPE`, no scope-audit issues. The unchanged strict timing report remains FAIL on lifetime bit 0x8; the limited observational conclusion below is retained. `RESULT.md` combines this with the numerical performance and other open verification items.

This is a separate audit of **newly observed DP errors within one recorded capture**. It does not replace analyze_stream_timing.py, edit any saved timing report, waive a fault bit, or claim unconditional acceptance.

## Production evidence and limitations

The sealed radar_display.c sets fault bit 8 from the lifetime tx_fifo_overflow latch. Its HPD interrupt wrapper reads register 0x0110, ORs the result into that latch, and increments TX_fifo_observations when the read is nonzero. Only HPD IRQ/EVENT are enabled in this application. Therefore this count is an **ISR observation count**, not an exact FIFO-event counter.

The display diag command separately prints the counters and then directly reads 0x0110; that direct read is not added to the ISR observation count. The sequence is not atomic. If an interrupt reads a new event after the counter has been printed but before the diagnostic register read, a single diagnostic can miss that new observation. Equal counters and two zero register readings cannot eliminate this race.

AMD's [2025.1 PS DP driver register definitions](https://raw.githubusercontent.com/Xilinx/embeddedsw/xilinx_v2025.1/XilinxProcessorIPLib/drivers/dppsu/src/xdppsu_hw.h) identify the PS FIFO register at 0x0110 and the interrupt registers. AMD's [DP TX subsystem AUX/register documentation](https://docs.amd.com/r/en-US/pg199-displayport-tx-subsystem/AUX-Channel-Interface) documents read-clear behavior for that PL core. The latter is **not used as proof of PS hardware read-clear semantics or complete PS interrupt coverage**. The precise PS register access behavior remains outside the evidence retrieved for this report.

Consequently the strongest result emitted by this tool is **NO_NEW_DP_ERROR_OBSERVED_IN_SCOPE**.

It means the preserved application observations contain no new error under the checks below. It does **not** prove zero physical DP faults, zero events, or absence of an unrecorded debugger/MMIO read. The report always retains exact_zero_event_proof=false, overall_unconditional_acceptance=false, and the original strict timing status. Full CPU coverage also remains NOT_PROVEN.

## Required scope and checks

The tool requires the completed stage, both complete diagnostics, the original timing audit and its hashed inputs, sealed launch input identities, and the single-owner recorder's raw bytes, RX/TX events, and command replies.

- Both 0x0110 reads must be zero. A nonzero initial read leaves scope incomplete; a nonzero final read fails.
- HPD and main stream must remain enabled. ISR/EISR errors must be zero; all timing/control/sink state must remain consistent.
- Lifetime FIFO latch and observation count must be identical. Any growth of the same already-set fault bit's count fails. Count reset/wrap or an unexplained final fault mask is not accepted.
- Every byte from the beginning of the initial diagnostic through the end of the final diagnostic must be covered by contiguous recorder RX events. Replies must match those actual bytes.
- Exactly two complete diagnostic reads must bracket one START and STOPPED. Additional reads, resets, unreviewed commands, runtime fault text, missing replies or changed saved diagnostics invalidate the scope.
- Page requests, status queries and the stopped cached scene export are allowed. This does not validate KEY3.
- Original timing input hashes are rechecked. All consumed files receive SHA-256 records; the stable raw byte range also receives its own offset/end/hash. The full recorder is append-only, so its recorded full-file hash is a snapshot at the reported byte length.

The timing gates are copied without reinterpretation. In particular, the 600-second gate cannot pass on the 16- or 64-frame trials.

## Usage

Use a new output path; the tool refuses to overwrite an existing report. Example single-line command:

    python /software/tools/audit_stream_dp_scope.py --stage /software/verification/20261102_stream_scene_01/board_checks/stream620_01 --session /software/verification/20261102_pl_board_01/board_sessions/live_03 --timing-audit /software/verification/20261102_stream_scene_01/board_checks/stream620_01/timing_audit.json --launch-identity /software/verification/20261102_stream_scene_01/startup/launch_identity.json --display-source /software/radar_runtime/src/display/radar_display.c --output /software/verification/20261102_stream_scene_01/board_checks/stream620_01/dp_scope_observed_audit_01.json

The 16-frame setup contains a nonzero initial 0x0110 read; it is not retroactively certified. The 64-frame run has both reads zero, unchanged observation count 1 and lifetime latch 1, and complete recorder provenance; its independent report is board_checks/stream64_01/dp_scope_observed_audit_01.json. Its original timing report remains FAIL due to lifetime fault bit 8. This short trial also fails the duration gate.

## Host verification

tests/test_stream_dp_scope.py uses synthetic fixtures only. It covers a pre-existing sticky bit, new same-bit observations, a new register error, nonzero initial state, counter reset, every other fault class, hidden intermediate reads, restart commands/banner, incomplete diagnostics, missing START/STOP, clean lifetime state, raw-byte gaps, tampered snapshots, input hashing and preservation of the original failure. Fourteen cases passed. No firmware, hardware, startup script or historical report was changed.
