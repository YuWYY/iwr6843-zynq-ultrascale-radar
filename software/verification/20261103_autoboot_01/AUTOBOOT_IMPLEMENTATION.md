# Fixed-installation automatic startup

The user selected power-on automatic acquisition and the third display page.
This is a new PS startup policy on the existing successful acquisition/display
code, not the unchanged v1.11.5 ELF. The original source files edited here were
copied and SHA-256 compared under `autoboot_source_baseline/` before modification.
The parent task separately preserves the complete v1.11.5 project baseline.

`RADAR_AUTOBOOT_ENABLE` is a CMake option, ON by default for this deployment.
Configure OFF to retain the original manual SAFE console. No FPGA, platform,
BSP, FSBL, radar profile, receiver, algorithms, UI layout or KEY3 logic is
changed by the automatic startup module.

The fixed contract is the previously user-confirmed F20 installation and the
continuous 3TX/4RX profile `ba6ddefbcaf3defb`. `confirm io` records that deployment
approval; it is not a runtime voltage measurement. Installation or radar
firmware changes require review of this boot policy.

The ordinary SW0/SW1, DMA cold-quiescence and memory checks run first. After a
2-second cooperative settle, `radar_autoboot.c` selects PL_BASELINE, decoupled
display, STREAM and the exact continuous profile. It then uses the existing
`confirm io`, `dma qualify`, `connect`, `check10`, `reset3`, profile confirmation
and preflight APIs. Each step checks the resulting state; a command being
handled is not considered success. DMA permission is checked through the
original epoch-aware API. CHECK10 and RESET3 must each produce a fresh completed
batch, and the original preflight checks the current reset/version identity.
CHECK10 has a 40-second total deadline, matching the successful board script:
ten individually valid 3-second replies plus TX, gaps and console draining can
exceed 30 seconds. This is an upper bound, not an added normal-start delay.

Raw HPD must be stable for 500 ms within a 30-second deadline. It does not invoke
AUX or repeated training. The existing `display init` implementation is called
once. That existing AMD driver path contains bounded synchronous AUX/training
calls and the existing 100 ms delay; the automatic state machine does not add
blocking waits or retries. RF is still off during this call. The first real
DPDMA descriptor acknowledgment must then remain healthy for 500 ms before TOP
is requested and the normal `scope live` API is invoked.

Startup PASS requires a newly verified transport frame, a processed result and
a real display acknowledgment. It is not emitted for training success alone.
It does not establish sensing accuracy or correctness of persistent boot media.

Historical DP TX FIFO startup bit 8 is retained and reported. The first real
descriptor acknowledgment freezes the existing ISR nonzero-observation count.
New observations during the following 500 ms stability window or automatic
LIVE operation trigger a normal stop; fatal/DPDMA errors always reject startup.
This counter counts ISR observations, not uniquely identified FIFO events, so
the rejection is conservative. The old startup warning is neither cleared nor
ignored for all future activity. Explicitly stopped acquisition does not trigger
an automatic restart or spurious monitoring action.

Any failed admission, native error, missing monitor, missing acknowledgment or
time reversal latches the failed stage and error. A live capture receives one
normal stop request and continues draining through the existing main loop;
pre-capture jobs are canceled through the native SAFE paths. No unowned buffer
is reused, no entire GPIO peripheral reset is added, and boot never retries.

Serial controls:

- `autoboot status`: cached boot phase and retained failure.
- `autoboot cancel`: cancels a pending automatic startup.
- Read-only status/help/profile/preflight queries do not cancel startup.
- Explicit control/configuration commands cancel pending startup first and
  return control to the existing dispatcher. Explicit `safe`/`disable` retain
  the native emergency-cancel path, including Ctrl-C translated to `safe`.
  Explicit `scope stop` retains normal boundary-stop behavior. Each is issued
  once to its existing owner, even while awaiting the first displayed frame.
- After startup succeeds, existing serial capture/display controls operate as
  before; there is no background automatic restart. The first explicit control
  or configuration command after DONE ends this boot's DP monitoring while
  retaining its result and counters. Thus a manual stop, display reinit and
  subsequent manual live session cannot inherit an obsolete FIFO baseline.
  Read-only status queries do not end monitoring.

## Targeted host evidence

`tests/test_autoboot.c` compiles the production startup FSM with service seams.
Both plain GCC and GCC with undefined-behavior sanitizer passed. The repeatable
command is `sh verification/20261103_autoboot_01/checks/autoboot_host/run.sh`
from the project root in WSL. The executable is also registered with CTest as
`deployment_autoboot_state_machine`.

Coverage includes successful ordered launch; rejected or unchanged command
results; wrong profile; rejected IO approval; DMA timeout; stale CHECK10 count;
preflight refusal; no/unstable HPD; failed single DP training; missing actual
descriptor ACK; link loss; refused LIVE; no fabricated first frame; failed
first-frame deadline; canceled DMA/live startup; read-only serial queries;
explicit configuration cancellation; backward time; and no automatic retries.
FIRST_FRAME tests distinguish explicit safe/disable emergency cancellation
from normal scope stop and prohibit issuing both paths or duplicate requests.
The manual stop/display-off/init/live lifecycle after DONE is checked to avoid
applying the old startup FIFO baseline to a later manual run.
Tests also retain startup DP bit 8 while rejecting new FIFO observations during
stabilization and after the first verified frame.

These seams do not prove board behavior. Remaining hardware checks include
cold boot without JTAG, correct boot medium and boot switches, FSBL loading and
normal handoff, exact artifact identity, radar reset/CLI behavior from a true
cold boot, display HPD before initialization, DP training/actual visible page,
continuous real acquisition, and manual serial stop. No KEY3 test is planned.

## Independent FSBL observations

The previous `startup/single_launch.tcl` programs PL with JTAG, stops at
`XFsbl_Exit`, then resets A53 and downloads the application. It does not prove
bootROM media loading, FSBL bit loading, or ordinary application handoff.
The exported old FSBL BSP has no QSPI/SDPS xparameters matches; final media
driver macros/symbols and the actual board storage must be checked before using
it for persistent boot. QEMU PMU firmware is not a board image.

FSBL's temporary bitstream address is `0x00100000`, inside ordinary application
memory. A normal boot image must load the bitstream before the application so
bitstream staging cannot overwrite an already loaded application. The existing
FSBL exit assembly disables MMU/caches and its handoff code cleans caches, but
actual handoff still requires cold-media verification.
