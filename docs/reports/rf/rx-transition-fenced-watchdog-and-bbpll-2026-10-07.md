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
- libbladeRF requirement-policy test passed; production shared-library build
  passed.
- Test-build xA4 runtime BBPLL-loss injection passed for RX1, RX2, and RX_X2:
  each emitted an invalidation before the blocked sync read returned, exposed
  no IQ after revocation, then recovered only through a fresh explicit epoch.
- A 20 ms positive-timeout live run passed RFPLL, ENSM, BBPLL, and FPGA epoch
  timeout/recovery cases in RX1, RX2, and paired RX_X2. The 20 ms test variant
  keeps the old FPGA image below its no-progress threshold; it verifies host
  fail-closed behavior, not the new FPGA watchdog fix.
- Python RF event notification tests passed (11 tests).
- Existing production xA4 transition-validity test passed.

## Qualification limit

The connected xA4 still runs FPGA v0.16.1, which does not contain the new
`rx_epoch_discard_active` watchdog behavior. Quartus is not installed in this
environment, so the modified FPGA has not been synthesized or loaded. The
long-timeout recovery case must be rerun on a synthesized image to qualify
this specific FPGA fix. No timeout or sample discard is accepted as evidence
that IQ is valid.
