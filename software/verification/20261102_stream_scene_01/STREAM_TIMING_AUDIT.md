# STREAM timing evidence format

Use `python tools/analyze_stream_timing.py RUN_LOG EXPORT_LOG... --output REPORT.json` after capture has stopped, the last DP acknowledgement has been serviced, final `scope status` has been saved, and **all** retained telemetry pages have been exported. The files must describe one complete run in saved order. Startup configuration time is outside the measured acquisition window. No frame is removed as warm-up and no config period is treated as a clock.

The fixed 10-minute gate requires at least 600 seconds between the first and last recorded DMA frame observations. A 620-second collection after the first mapped frame provides margin; the tool never rounds a shorter run up. Rates use the complete interval from first DMA observation through the latest publication or first DP acknowledgement, whichever is later. Thus a late short burst of display updates cannot masquerade as a full-run 9 Hz display rate.

## Required production records

- One `[STREAM START]` with `backend=PL_BASELINE` and one `[STREAM STOPPED]` with verified/processed/presented/remaining/residual/error/records.
- Final idle `[STREAM STATUS]`: running, epoch, verified, processed, presented, ready, owners, high, error, records, overwritten, skipped_display, pending_display, crc_errors, hsi_errors, dma_errors, metadata_errors, dp_errors, dp_active, health_cpu_us, health_max_us.
- Each telemetry page: `@STREAMBEGIN first count records overwritten epoch`, ordered `@STREAM` rows, `@STREAMEND next records epoch`.
- Each row: record, frame, epoch, seq, crc, flags, stages, raw, lease, task, tasklease, first, last, mapped, uncertainty, observed, ready, front, submitted, done, published, ack, cpu, render, queue, high, revision, page, error, bytes, hz, tstatus, ackseq, ackrevision, ackframe, renderbegin, renderend, queued, coalesced.

`record` is the 1-based RAM log sequence. Export `first` and `next` are zero-based indices. `crc`, `flags`, `stages`, `tstatus` and `dp_errors` are hexadecimal; other numeric fields are decimal. `first` and `last` in a frame row are SYS clock ticks; the tool never directly subtracts them from PS microseconds.

The final status may have more presentations than the STOPPED line because the last queued descriptor may complete after capture stops. It must not have fewer. Verified/processed/record totals must stay unchanged. Explicit display merging is counted by the complete set of published frames without a first ACK below the last actually presented frame. Unacknowledged tail frames remain pending and prevent PASS. The raw lifetime `coalesced` counter is reported separately and cannot explain missing frame records.

## Metrics and hard evidence gates

- All records 1..N present, no overwritten or conflicting exports, no record/transport/frame sequence gaps, unique RAW/task leases, and one epoch.
- Exact 399,360 bytes, valid HSI/CRC flags, all processing stages completed, transport status clear, consistent clocks and monotonically increasing observed/published/mapped times.
- First actual DP ACK identity must match frame and data revision. Repeated page ACKs do not increase the count of new frames. Render, queue and ACK timestamps must be ordered.
- Acquisition and processing ≥9.5 Hz; unique newly displayed frames ≥9 Hz.
- Display interval nearest-rank P95 ≤150 ms and maximum ≤250 ms, including the beginning and end of the run.
- Latency is **mapped SYS first accepted handshake → first actual DP ACK**. Every contributing frame must have transport/time flags and mapping uncertainty ≤1,000 µs. Both nominal and conservative upper-bound latency (`ack - mapped + uncertainty`) are reported; the upper-bound P95 must be ≤300 ms.
- Maximum recorded per-frame kernel CPU ≤80 ms. This field includes RX copy/first verification, preparation/second CRC, driver calls, spatial/scene work, and render through queue. Aggregate health CPU and maximum health call are reported separately. Main-loop, interrupts, UART and uninstrumented admission work mean the full per-frame CPU budget is **NOT_PROVEN**.
- Queue monitoring: last-quarter mean ready depth must not exceed first-quarter mean by more than one frame; no record reaches the overload depth of eight; final ownership/pending counts are empty. These are explicit finite-run criteria, not a mathematical guarantee of queue stability.
- Scoped error counters and recorded STREAM/DPDMA error lines must all be clear. Missing status fields, missing export envelopes or missing records never pass.

## JSON interpretation

Schema `M2_STREAM_TIMING_AUDIT_V1` contains:

- `status`: `PASS_INSTRUMENTED_GATES`, `FAIL_GATES`, `FAIL`, or `INCOMPLETE`.
- `definitions`: clocks, window, percentile rule and zero trimmed records.
- `coverage`: expected/exported/unique records, first/last frame, epoch, presentations, merges and pending tail.
- `metrics`: rates, spans, full-run interval/latency/CPU distributions and queue statistics.
- `cpu_coverage`: included work, separate health aggregate, uncovered work, and explicit `full_cpu_80ms_gate=NOT_PROVEN`.
- `gates`: each measured numeric gate independently.
- `issues`: exact line, integrity/error code and details.
- `final_status`, `stopped_summary`, `input_files` with file hashes.

The exit code is zero only for `PASS_INSTRUMENTED_GATES`. This status does not establish total CPU utilization, physical detection/classification accuracy, KEY3 function, or RF/ADC-to-screen latency.

The fixtures in `tests/test_stream_timing_analyzer.py` are synthetic. They exercise short runs, missing/overwritten records, incorrect metadata, untrusted maps, first-gap retention, ACK identity and missing/coalesced ACK accounting, uncompleted tail, explicit errors, incomplete envelopes and multiple-run selection. Their PASS is a parser/test result, not a recorded-board result.
