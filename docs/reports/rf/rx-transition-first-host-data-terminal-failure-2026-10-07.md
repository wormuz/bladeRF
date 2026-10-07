# Fail first-host-data wait on terminal stream mismatch — 2026-10-07

## Change

When a transition requires first host data, a known terminal incompatibility
now fails the wait immediately. RX channel-mask mismatch, an unsupported
RX_X2/RX_X1 layout, and unsupported stream format set a failure under the same
lock used by the waiter and signal its condition variable. The waiter returns
`BLADERF_ERR_UNSUPPORTED`; the existing reason-coded RF event reports the
specific mismatch. Recoverable stale-epoch, timestamp, and transport
withholding remain notifications and do not prematurely terminate the wait.

The failure state is reset for each new transition. It never certifies IQ:
only the existing FPGA epoch plus host-validated META path can produce the
first-host-data success event. Deadline expiry remains a failure outcome.

## Validation

- Production library and `libbladeRF_test_sync_epoch_traversal` build.
- Native traversal verifies terminal channel-mask and RX_X2 layout mismatch
  set fail-closed state, while the corresponding reason-coded event is still
  retained. Unsupported format notification also exercises the terminal
  failure path.
- `libbladeRF_test_sync_epoch_traversal` exits successfully. Its three
  sync-buffer timeout diagnostics are intentional timeout-path cases.
- `git diff --check` passes.
- No attached-board run was needed or performed; this change is host-side
  waiter signaling and does not alter FPGA or RFIC programming.

## Remaining scope

This closes deterministic stream-configuration waits. It does not prove
analog settling beyond the RFIC status events, and it does not replace the
separate RX1, RX2, and RX_X2 hardware transition/throughput qualification.
