# Sync RX epoch recovery test fixture state

## Finding

The `test_sync_epoch_traversal` recovery case timed out one `sync_rx()` call,
then injected a new completed ring buffer into its hand-built fixture. The
timeout left the parser in `SYNC_STATE_CHECK_WORKER`. Since this fixture has
no `sync_worker`, the next read dereferenced a null worker. GDB located the
crash at `sync_worker_get_state(w=0x0)`; this was a test-fixture modeling error,
not a production stream failure.

## Change

Immediately before injecting the post-timeout completion, the fixture now sets
its state to `SYNC_STATE_WAIT_FOR_BUFFER`, modeling the already-running
production worker whose next USB completion the test injects. The recovery
assertions remain unchanged: stale epoch data is discarded and only the
current epoch/timestamp data reaches the caller.

## Validation

- Rebuilt `libbladeRF_test_sync_epoch_traversal` and ran it successfully.
- `host/misc/run_rx_transition_policy_test.sh`: PASS.
- `host/misc/run_rx_epoch_metadata_test.sh`: PASS.
- `hdl/quartus/qcheck`: clean (existing informational notes only).
- `git diff --check`: PASS.

This fixture correction does not change runtime behavior or the RX validity
contract. A timeout still returns failure and cannot certify IQ.
