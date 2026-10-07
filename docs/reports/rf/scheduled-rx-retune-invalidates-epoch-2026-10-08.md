# Scheduled RX fastlock retune invalidation — 2026-10-08

## Defect

`bladerf_schedule_retune()` wrote a future RX fastlock recall directly to the
board callback. Unlike direct frequency changes, it skipped the common RX
invalidation path, so event consumers could retain an earlier IQ-valid
certificate while NIOS later changed the LO. The board callback blocked this
API only while an epoch filter or transition was active; outside that mode,
the asynchronous RF mutation had no completion or first-valid notification.

## Change

The public API now validates channel and bladeRF 2 quick-tune arguments, then
revokes RX data validity with `BLADERF_RF_INVALIDATE_FREQUENCY` before
queueing an RX recall. If the fence fails, no queue write occurs. Successful
queue writes release the setter reservation but do not certify data. The
existing event-driven transition path cancels any outstanding scheduled RX
recalls before starting its own epoch; samples remain invalid until that
transition succeeds. TX scheduled retunes remain independent of RX
invalidation.

The board policy now blocks ordinary recalls only while a transition is
pending. It permits a public recall after the common invalidation has revoked
the current epoch, while the FPGA fence and sync parser keep RX data withheld.

## Verification

- Production `libbladerf_shared` and
  `libbladeRF_test_rx_shared_clock_invalidation` build successfully.
- The native test verifies RX invalidation happens before the callback,
  failed invalidation prevents the scheduled write, and TX scheduling does
  not invalidate RX.
- `hdl/quartus/qcheck` passes and now guards this call ordering and the
  transition-pending board policy.
- No hardware scheduled recall was run. Runtime NIOS recall timing and
  recovery into a subsequent certified epoch remain for board qualification.

No timeout or discard behavior was added. Only a successful event-driven
transition can restore the RX IQ-valid certificate after a scheduled recall.
