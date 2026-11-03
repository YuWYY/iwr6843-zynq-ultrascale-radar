# v1.10.1 STREAM integration contract

This is the implemented interface, not a statement that all performance or physical accuracy gates passed. See `status.json` and the final board report for measured results. FINITE is retained; STREAM requires the matching ABI `0x00010001` and explicit PL_BASELINE selection. No fallback occurs on a capability mismatch.

## Data ownership

The capture DMA uses 128 SG descriptors, seven per exact 399,360-byte radar frame and at most 18 preposted transport frames. Software reclaims/refills descriptors; cyclic overwrite is not enabled. Each transport frame is copied in bounded chunks into one of sixteen 512 KiB RAW slots. Copy CRC, all 96 HSI records, transport metadata and exact length must pass before publication. Preparation checks CRC again.

RAW slot state and monotonically changing leases distinguish free, writing, verified and processing ownership. Frame identity includes capture epoch, transport sequence and global frame ID. A slot cannot be reused while owned. Backlog at eight or oldest waiting data over 500 ms triggers a controlled unsuccessful stop; it does not restart RF automatically. Error recovery cannot release memory still owned by an undrained DSP transaction.

Two 3 MiB DSP task slots start at `0x02600000` and `0x02900000`. The single PL engine executes in order while PS can prepare the next slot and postprocess the prior result. Task layout and leases are checked. Completed immutable display snapshots occupy `0x02C00000` and `0x02C80000`; scene, cached weights, coordinator and telemetry use the existing reserved DSP workspace. Ordinary image ends below 2 MiB. DMA, RAW and DP reservations are unchanged.

Only obsolete display requests may merge. Capture and processing frames are not silently dropped. Each publication has a frame/data revision; the first actual DP acknowledgement must match both. The final data revision is retained until acknowledged, before publishing a separate stopped-state revision.

## Start, stop and commands

Only idle state permits `capture mode finite|stream` and `capture stream 16|64|continuous`. Profile identities are respectively `dba7d89f0e179af4`, `add77002be6cf7ce`, and `ba6ddefbcaf3defb`. The waveform geometry remains 3 TX / 4 RX, 256 samples, 32 loops, 100 ms nominal frame period, TX slots [0,2,1]. Profile confirmation does not attest unknown physical input timing.

After explicit admission, `scope live` performs reviewed configuration, starts DMA/time epoch, arms capture and starts RF once. `scope stop` requests a PL packet-boundary stop, confirms sensorStop and drains complete frames and metadata. With unused preposted descriptors, a strict drain proof authorizes one native software reset; an exhausted finite tail retains HALT then reset. Both require confirmed native reset/Halted/ring rebuild before metadata END. Residual data is separately reported. Failure does not silently start another epoch. See `STOP_PREFETCHED_RING_FIX.md` for the preserved first-board failure and actual fix validation.

Use `scope status` for summary and error counters. After stopping, `scope telemetry FIRST COUNT` exports at most 256 of the 8192 retained RAM records per request. Export all pages for a complete-run audit; overwritten records prevent acceptance. `scope scene` exports actual track rules/features; `scope scene reset` resets installation-dependent state only when idle. Existing display page commands and KEY3 logic are unchanged; KEY3 was not tested.

## Time and measurements

The PL atomic SYS snapshot includes generation, 64-bit tick count and capture epoch. PS brackets snapshots and refreshes the map every 500 ms. A map expires after one second and rejects backwards/discontinuous time or mismatched epochs. Uncertainty includes an explicit 100 ppm engineering drift allowance, checked against successive anchors; it is not a calibrated oscillator certificate.

Only mappings with uncertainty at most 1000 us support reported latency. The endpoint definition is **SYS-domain first accepted FIFO-to-DMA handshake to first actual DP acknowledgement**. This excludes unknown ADC/RF-to-FIFO time. The analyzer reports both nominal latency and the conservative mapped upper bound.

`cpu` is the sum of instrumented copy/verification, preparation, driver, spatial/scene and rendered-frame work. It excludes some main-loop, capture-service and console overhead. Health work is exported separately. Consequently the full 80 ms CPU budget is not established by this field alone; stable 10 Hz is not a substitute for complete CPU accounting.

## Evidence and limits

`checks/root_cpu/result.json`: table CRC versus the original algorithm and original/cached spatial results on synthetic and saved real data. `checks/stream_final_03/summary.json`: SG 16/64-frame behavior, wrap, leases, faults and coordinator integration. `checks/stream_ack_export/summary.json`: late final ACK and nontruncated full telemetry. Time-interface native and host evidence is summarized in `checks/timebridge_native_signoff.json`; matched platform/app result is `checks/platform_native/matched_stream_03/result.json`.

`SCENE_IMPLEMENTATION.md` defines tracking, rule evidence and reflection-map limits. `STREAM_TIMING_AUDIT.md` defines strict complete-run timing analysis. The slow-waveform candidate under `candidates/slow_idle100` is unselected and NOT_RUNTIME_READY; it is not a tested production mode. Space calibration, measured classification accuracy and surveyed wall positions remain pending. No ZIP or KEY3 test is part of this delivery.
