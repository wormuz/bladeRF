# RX transition fence and BBPLL state

Date: 2026-10-07

## Findings

Two transition validity gaps were addressed in this change:

- On the connected v0.16.1 FPGA, a 100 ms injected PLL timeout was followed by
  a sticky RX-fault aggregate during the next transition. `RF_LINK_STATUS`
  exposes only the OR of five RX writer fault causes, so the exact latched
  cause is not observable from this image. A likely source is the writer's
  2^22-clock no-progress watchdog (about 34–68 ms), because a retune fence
  intentionally suppresses data writes. This diagnosis is not yet confirmed
  on hardware.
- The host verified the AD9361 RF PLL and ENSM state, but not the baseband PLL.
  Runtime monitoring likewise had no BBPLL-loss invalidation event.

## Changes

The RX gate's `rx_epoch_discard_active` now reaches the RX `fifo_writer`. While
the gate is intentionally withholding data, the progress counter stays at
zero and no-progress fault classification is paused. Once the gate opens (or
leaves the fenced state after failure), the normal watchdog resumes. The
separate GPIF timeout, overflow, abort, and USB fault paths are unchanged.
The input defaults inactive so non-RX and legacy instantiations keep their
existing behavior. `PROGRESS_TIMEOUT_LOG2` is now a generic to permit a short,
deterministic watchdog test.

libbladeRF now checks AD9361 register `0x05e`, bit 7, as BBPLL lock when BBPLL
confirmation is requested; epoch-valid requirements imply that check. A
missing lock or unreadable status fails the transition and leaves IQ
invalidated. The runtime monitor reports BBPLL lock loss through the appended
`RX_BBPLL_LOCKED` event and the
`RF_INVALIDATE_RFIC_BBPLL_UNLOCKED` invalidation reason. Python exposes both
without treating the lock event as valid IQ.

The RX1/RX2 data gate remains one shared epoch because the AD9361 RX LO and
BBPLL are shared. It only opens after all enabled stream lanes are valid. The
existing gate test covers paired RX_X2 waiting for both lanes, while the live
transition checks below exercise RX1, RX2, and RX1+RX2.

## Verification

- GHDL: new `fifo_writer_epoch_fence_tb` passed; existing writer abort and
  enable testbenches passed. The new test holds a fence longer than its
  shortened test watchdog, verifies no sticky fault, then drops the fence and
  confirms no-progress detection resumes.
- `host/misc/run_rx_epoch_gate_tb.sh` now runs the epoch-gate, writer fence,
  writer abort, and writer enable testbenches together. The consolidated
  runner passed with GHDL 5.0.1 in this worktree. The gate bench explicitly
  verifies RX1-only, RX2-only, and paired RX_X2 epoch boundaries, including
  that a paired epoch waits for both enabled lanes.
- Quartus Prime 25.1std full flow passed for the xA4 Cyclone V `sweep`
  revision: Analysis & Synthesis 0 errors, Fitter 0 errors, Timing Analyzer
  0 errors, Assembler 0 errors. Across 32 setup and 32 hold corners, the
  minimum slack is +0.482 ns setup and +0.011 ns hold; TNS is 0.000 in every
  corner. The minimum hold margin is small and remains a timing watch item.
- The retained build is
  `hdl/quartus/work/adr0207-rx-fence-xa4/output_files/sweep.rbf` (2,632,660
  bytes, SHA-256
  `b8710635e9dc2ad202bd5d239fb2fe2adb1af2863e2878ec0ca3bbc9bb4be3e4`).
  Its fit, STA, and full-flow reports are in the adjacent `output_files`
  directory. It was loaded to the connected xA4 with `bladeRF-cli -l`; the
  CLI used `BLADERF_SKIP_FPGA_SIZE_CHECK=1` because this unit's FPGA-size
  readback is unsupported. The RBF is an xA4 image of the expected fixed
  2,632,660-byte size. It was not written to flash.
- Quartus exposed an integration error missed by unit testbenches: the RX
  entity read its `rx_epoch_discard_active` output while wiring the FIFO
  writer. An internal `rx_epoch_discard_active_local` signal now drives both
  the output port and writer input. The successful full flow includes this
  fix.
- libbladeRF requirement-policy test passed; production shared-library build
  passed.
- Focused policy, metadata-epoch, sync epoch-traversal, and FPGA epoch-status
  identity tests all passed. The metadata test runner now puts this worktree's
  public header directory first, avoiding accidental compilation against an
  older system-installed `libbladeRF.h`.
- Test-build xA4 runtime BBPLL-loss injection passed for RX1, RX2, and RX_X2:
  each emitted an invalidation before the blocked sync read returned, exposed
  no IQ after revocation, then recovered only through a fresh explicit epoch.
- A 20 ms positive-timeout live run passed RFPLL, ENSM, BBPLL, and FPGA epoch
  timeout/recovery cases in RX1, RX2, and paired RX_X2. The 20 ms test variant
  keeps the old FPGA image below its no-progress threshold; it verifies host
  fail-closed behavior, not the new FPGA watchdog fix.
- Python RF event notification tests passed (11 tests).
- Existing production xA4 transition-validity test passed.

## Hardware qualification

The board reports FPGA version v0.16.1 for both the previous and newly loaded
image because this build does not change the encoded version string. The new
image was verified by loading the retained RBF and exercising the changed
watchdog behavior directly. `rx_transition_validity_live` passed, and
`run_rx_epoch_positive_timeout_live.sh` passed on RX1, RX2, and paired RX_X2.
For RFPLL, ENSM, BBPLL, and FPGA epoch stalls, each injected 100 ms timeout
returned a timeout error, withheld IQ, then recovered only after a fresh
event-driven transition. Thus the fence remains valid for longer than the old
34–68 ms no-progress watchdog interval without a sticky writer fault.

Timeout or sample discard is never accepted as evidence that IQ is valid; only
the successful post-timeout event-driven recovery established a valid epoch.
