# Scene processor and third-page display

Final board update: `board_checks/stream620_fixed_01` now contains all 6213 exported records over 621.224 seconds, successful normal stopping and subsequent LIVE restart. Numerical performance targets were met; the monitor's TOP VIEW and advancing frame number were confirmed by the user. Final scope, CPU/DP limitations and measured metrics are in `RESULT.md`. These results do not establish physical tracking or classification accuracy. Earlier staged observations below remain historical evidence.

## v1.10.1 identity and bounded board evidence

The current application is M2-SW4.2-v1.10.1. Its native build in checks/application_native/stop_fix_02/result.json is PASS. Its ELF SHA-256 is 9f6f371977ccd7316d6e98b2611135c64bd53dc9fc3cf3288f256689ee2f7b46; startup/launch_identity_v1101.json seals this executable and the unchanged bit/XSA/FSBL identities. The normal-memory end remains 0x001BA3C0. This supersedes only the earlier executable/version reference below.

Comparison of the original and v1.10.1 built-source manifests, followed by current-file hashing, confirms that all nine scene, spatial and scope-renderer source/header files are unchanged. The changed application files are capture/radar_stream.c, capture/radar_stream.h, config/radar_contract.h and processing/radar_stream_scan.c. This is a stream-stop correction; it does not introduce a new tracking, classification, bearing or rendering algorithm.

The root-owned board run board_checks/stream_stop_fixed_01 recorded 91 verified, processed and actually presented frames, zero residual/pending frames, no CRC/HSI/DMA/metadata error, and a completed stop via RUN_TO_RESET with 119 unused descriptors. These observations exercise continuous capture stopping, not physical classification accuracy or the full ten-minute gates.

Its new independent dp_scope_observed_audit_01.json reports NO_NEW_DP_ERROR_OBSERVED_IN_SCOPE, with complete before/after diagnostics, unchanged FIFO observation count, and recorder-byte provenance. The original strict timing audit still reports FAIL for the lifetime DP mask 8. Non-atomic DP snapshots and incomplete total-CPU instrumentation remain explicit limitations; there is no unconditional full acceptance.

The earlier 6,213-frame run board_checks/stream620_01 is retained as a failed stop attempt (HALT_TIMEOUT_QUARANTINED, error -210), even though its acquisition-period metrics met the numerical targets. Its failure telemetry and report are not superseded by the short correction check. The separate stream620_fixed_01 run was still in progress when this section was written; its outcome must be taken from its own completed evidence.

The documentation/audit subtask did not edit application sources, build/launch scripts or CURRENT_RUNTIME.md, and performed no hardware action or KEY3 test. Physical target accuracy, classifications and wall-line accuracy remain unvalidated.

## Original native review (historical, preceding v1.10.1)

Status at the final source review on 2026-11-02: the scene pipeline is integrated in the production STREAM path, its targeted host tests pass, and the matching native application build `matched_stream_03` is PASS. Physical labels, positions, wall lines and the ten-minute live-performance gates remain unvalidated by this document. Synthetic fixtures are restricted to host tests. No KEY3 test or hardware action was performed for this documentation review.

## Implemented, offline checked, and still awaiting hardware evidence

| Area | Current evidence | Boundary |
|---|---|---|
| STREAM spatial → scene → display path | Production source integration, host coordinator tests and matched native build | Does not establish live throughput or display coexistence |
| Up to eight XY tracks, measured trails, prediction expiry and experimental rules | Targeted synthetic scene tests, GCC warnings-as-errors, ASan and UBSan | Does not establish eight-person separation, identity accuracy or classification accuracy |
| Independent horizontal-only bearing | Implemented in production spatial processing; scene tests exercise flag admission without fabricated XY/Z | Actual left/right bearing and elevation validity still need known-position targets |
| Third-page appearance and state handling | Eleven production-renderer host fixtures, framebuffer guards and visual review | Host PNGs are not photographs or verification of the monitor |
| Matching XSA/BSP/application and memory allocation | `checks/platform_native/matched_stream_03/result.json` and `elf_layout.json`: PASS; normal memory ends at `0x001BA3C0`, below `0x00200000` | Native compilation and file provenance are not board execution |
| Startable product identity | `startup/launch_identity.json` seals bit/XSA/FSBL/ELF inputs; their four hashes matched the files during this review | A sealed launch identity does not assert successful download or launch |
| Accuracy, labels and walls | Explicit `UNCALIBRATED`, `EXPERIMENTAL LABELS`, `HUMAN?`, `WALL?` and quality states are present | No measured error distribution, false-positive/false-negative rate, or wall survey comparison yet |

During this documentation-only review, all 103 application-source entries in `checks/platform_native/matched_stream_03/built_application_source.json` matched the current files. The scene/renderer/test hashes in `checks/scene_checks.json` also matched. The sealed ELF SHA-256 is `ae41c21136bf18c424a5979dcc3c9cadde06db16aa89d7848133f6b08f35540d`. This review changed only this document; application sources, build and launch scripts, artifacts and `CURRENT_RUNTIME.md` were not edited.

## Integration contract

- `radar_scene_workspace_bytes()` currently returns 167,664 bytes. Production `app/main.c` passes the reserved DDR address `0x02D00000` and a 1 MiB allocation to initialization. The scene implementation's own limit remains 256 KiB and alignment requirement is at least eight bytes. No heap or large normal-BSS arrays are added.
- `init / begin / step / cancel / result`: the source spatial frame remains immutable until completion. `step` performs one bounded tracking pass, one 121-cell heatmap row, or one bounded line-fitting trial. Publish the returned 5,672-byte scene snapshot together with its matching spatial/range results.
- Supply actual frame sequence, capture epoch, installation epoch, waveform hash, calibration identity, observed timestamp and trust state. The source timestamp/calibration must match. Duplicate/backward frame identity or time is rejected. Unknown time produces explicit unavailable tracking. More than 500 ms without an observation clears continuity.
- Capture/config/calibration/installation changes reset tracks and the reflection map. Track IDs are not recycled across resets, except eventual integer rollover. The installation epoch is the explicit manual move/reset seam; broad reflection loss is only an additional heuristic.
- Call `radar_scope_render_scene(..., spatial, scene)`. Its session/time check prevents mixing frames. It is a pure painter. Legacy `radar_scope_render` remains source compatible and draws an unavailable scene until a scene snapshot is supplied.
- `radar_scene_reset(..., RADAR_SCENE_RESET_INSTALLATION)` is the explicit reset seam. Predictions, repeated frames and painting never advance measurements or category evidence.

The actual STREAM call order in `processing/radar_stream_scan.c` is:

1. Import the PL distance power and call `radar_spatial_begin_config(..., frame.cfg_sha256)` with either the explicitly installed calibration or `NULL`.
2. Import the PL candidate list and complex Doppler rows; the precomputed calibration × TDM product is applied once during row import. The spatial stage then produces its heatmap and angle results.
3. Construct `RadarSceneContext` from the same frame's capture epoch, frame ID, mapped timestamp/trust flag, configuration hash, installation epoch and the spatial result's calibration identity.
4. Run the cooperative scene steps; only after DONE copy range, spatial and scene into the publication together. Display reads its own snapshot through `radar_scope_render_scene`.

`radar_spatial_begin_config` requires the actual 64-character configuration identity. A supplied calibration must have matching array/configuration identities, finite nonzero complex channel coefficients, a finite range bias and a valid measured flag. A measured calibration cannot be named `UNCALIBRATED`. With `NULL`, production explicitly installs unit channel coefficients, zero bias, the current configuration hash and `UNCALIBRATED`. A radar-side compensation command is not treated as calibration already applied to raw ADC data.

The production main loop supplies a scene snapshot only when STREAM mode is selected. FINITE remains a fallback acquisition path; its lack of a current scene snapshot must not be interpreted as a failed hardware bearing estimate. A different capture epoch clears temporal state, so short finite batches do not accumulate a hidden ten-second wall map across runs.

## Horizontal bearing and unknown height

`RADAR_POINT_BEARING_VALID` is bit 64 in `processing/radar_spatial_types.h`. When a candidate lacks valid XYZ and is not at the Doppler edge, production `finish_point()` separately searches the eight-element horizontal subarray over the existing −60°…+60° projected-angle grid. It accepts the horizontal bearing only if normalized subarray coherence is at least 0.5 and the selected beam is not either search edge. The accepted 3D solution is left unchanged.

This bearing can remain valid when the twelve-element 3D result has low coherence or an elevation boundary flag. It gives `horizontal_angle_deg`; it does not set `XYZ_VALID`, replace missing coordinates with zero, or estimate elevation. The scene processor accepts that independent flag and retains the observation in `bearings[]`, while physical XY tracking still requires quality-qualified XYZ. The second page may overlay its measured range/projected angle. The third page reports `BEARING ONLY / NO XY POSITION`; it does not place that observation at a ground-plane coordinate. The fourth page continues to require valid XYZ.

## Track and rule definitions

Only finite, quality-qualified XYZ points enter physical XY tracking. The independently qualified horizontal-bearing flag is retained as bearing-only metadata, even if elevation/3D coherence is invalid. It never becomes a fabricated XY position or zero height.

DBSCAN uses 0.35 m and minPts 2 including the point itself. An isolated point requires SNR at least 15 dB and coherence at least 0.65 before it can enter a tentative track. Confirmation requires 3 observed hits in the latest 5 accepted frames. At most 8 tracks are retained; capacity rejection is explicit.

The constant-velocity XY Kalman filter uses actual elapsed time, observation variance 0.0144 m², acceleration noise 0.8, a squared-Mahalanobis gate of 9.21 and an additional 0.85 m distance gate. Deterministic globally closest-first association is used; it is not a claim that identities remain distinguishable during unresolved merges. Competing matches suppress nonrigid evidence and flag association ambiguity. Missing tracks have dashed predictions for at most 500 ms, then are removed. Trails contain only measurements from the last 2 seconds.

`RULES_V1` exposes bits and feature values, not probabilities:

- E: measured cluster extent 0.2–1.5 m.
- M: three consecutive measurements support filtered XY speed 0.1–2 m/s. No fractional interpretation of a Doppler bin is used.
- P: confirmed track, at least 2 seconds since its first measurement and at least 10 actual hits.
- N: three consecutive measurements with at least three points, extent-deviation exponential mean at least 0.04 m and radial-velocity-spread exponential mean at least 0.2 m/s. Feature smoothing factor is 0.2 per valid measurement.

`HUMAN?` requires M, P and N and at least three bits. E is supporting evidence, not mandatory. Otherwise a confirmed moving track can be `MOVING OBJECT`; a persistent currently measured track below 0.1 m/s can be `STATIC REFLECTOR`; all other cases remain `UNKNOWN`. Missing measurements immediately remove semantic qualification; prediction does not renew any counter. These rules can misclassify other moving, articulated objects and have no measured classification accuracy.

The rule score is exactly the number of set E/M/P/N bits, in the range 0–4. It is not a probability, confidence percentage, or measured recognition rate. A prediction clears M/N hit recurrence and publishes `UNKNOWN`; only new accepted measurements can rebuild those rules. The N rule is an engineering proxy based on cluster shape change and Doppler spread, not proof of nonrigid biological motion. A moving chair or an unresolved group of reflections can meet it. Unknown quality and ambiguous association remain visible rather than being reinterpreted as people.

## Persistent-reflection hypothesis

All 205×121 heatmap cells are considered. Fixed evidence requires power ≥−75 dBFS and at least 4 dB above the radial ±3-bin neighbors. Moving-track neighborhoods (0.5 m plus half cluster extent) are excluded from learning. No sparse-CFAR-point wall construction is used.

The rolling 10-second window has forty 250 ms occupancy buckets. A bucket counts as supporting a cell when at least one valid observation in it supports that cell; the support fraction is therefore **time-bucket occupancy, not a per-frame detection rate**. A map requires 10 seconds elapsed, at least 28 observed buckets and at least 70% cell support. Current-frame evidence is also necessary, so departed reflections cannot coast as current wall lines for ten seconds.

At most 256 deterministic reservoir-sampled supported cells enter bounded 96-trial line fitting, followed by total-least-squares refinement. A candidate needs at least six cells, length ≥1 m, residual ≤0.15 m and ≥70% occupied 0.25 m line intervals. Gaps over 0.35 m split the fit: only contiguous measured runs are drawn. Up to four lines can be returned. They are horizontal-projection hypotheses, not surveyed 3D wall planes or room completion.

Three consecutive observations with loss of more than 70% of at least 64 established persistent cells invalidate the map and tracks. This heuristic cannot guarantee detection of all radar moves or distinguish a large scene change from a move.

## Display and validation

Third page preserves equal X/Y scale, adds stable IDs, measured trails, dashed predictions, experimental labels, sampling time/age and rule evidence. More than four tracks use compact plot IDs with full sidebar labels. Bearing-only observations are called out separately. Cached/paused/error scenes are labeled. The other three pages retain their original meanings and the KEY3 module is unchanged.

Run `python verification/20261102_stream_scene_01/scripts/check_scene.py` from the workspace to rebuild tests and render fixtures. It records strict GCC, AddressSanitizer/UndefinedBehaviorSanitizer and production-renderer logs, image outputs and source hashes under this run. The host checks cover 3-of-5 confirmation, expiry, actual trails, separated crossings, label evidence/conflict, timestamp/context reset, independent bearings, persistent-line positives, intermittent/short/disconnected/empty-map negatives. These do not establish physical tracking, classification accuracy, monitor output or target hardware CPU time.

The final coordinator regression is separately recorded in `checks/stream_final_03/summary.json` (`passed=true`, UBSan, stable source snapshot). The scene-only and renderer evidence remains `checks/scene_checks.json`, `checks/scene_tests.log`, `checks/scene_sanitizer_tests.log` and `checks/scene_visual_review.json`. The newer native build and sealed launch identity supersede earlier compile attempts for product identity; they do not turn these host scenes into physical data.

## Manual scene inspection and installation reset

The following are existing console commands in STREAM mode. This document records their behavior; none was sent during this review.

1. Use `scope stop` and wait for `[STREAM STOPPED]`. Read `scope status` and confirm processing is idle and ownership is drained. A failure or retained owner must be resolved through its explicit recovery path before changing state; this scene command does not recover DMA or a failed source.
2. Before clearing evidence, `scope scene` exports the last cached `@SCENE`, each `@TRACK`, and `@SCENEEND`. It includes frame/epoch/time, calibration, flags, measured/predicted state, label, rule bits/score, last measured time, XY, filtered speed, extent, shape variation and radial spread. The export reads cached software results; it does not poll the radar or restart acquisition. It can return `[SCENE BLOCK]` while work or a previous export is active, or while the console lacks capacity.
3. After physically moving the radar, changing the installation, or deliberately starting a new map, issue `scope scene reset` while idle. It increments the installation epoch, clears tracks and the persistent map, clears the published scene snapshot, discards any installed software calibration and marks the result `UNCALIBRATED`. Expected acknowledgement: `[SCENE RESET] Installation=... , tracks/map cleared, calibration=UNCALIBRATED` (the actual formatter has no space before the comma).
4. `scope scene reset` does not start RF, change the selected waveform, reset the radar, or create calibration coefficients. Resuming the already admitted STREAM run uses the existing `scope live` workflow; the scene then needs fresh observations to confirm tracks and warm the map again. An active run rejects reset rather than silently stopping it.

`scope clear` is a broader retained-view/history operation and is not the installation-calibration reset command above. `capture clear` concerns retained RAW evidence/ownership and is also distinct. Keep these operations separate when preserving logs for a report.

## Remaining physical sign-off

The next board run must establish stable capture/PL processing/DP coexistence and measured timing using the complete stopped-run telemetry, not this document or synthetic fixtures. The performance tool's `PASS_INSTRUMENTED_GATES` is scoped to its measured work; full CPU coverage remains explicitly separate. Known-position reflectors are still needed for left/right/up direction, distance bias and spatial error. Whole-sequence people/object experiments and measured wall lines are needed for label and structure quality, including `UNKNOWN`, false positives, missed targets and merge/crossing failures. Until those checks exist, smoothing and a stable ID mean temporal consistency rather than improved absolute accuracy.

The separate slow-waveform files under `candidates/slow_idle100/` remain unselected and `NOT_RUNTIME_READY`. They do not change this scene pipeline's current 31 µs chirp/TDM model or constitute physical slow-motion validation. No KEY3 validation or ZIP delivery is part of this scene work.
