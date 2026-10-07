# Publish FPGA RX-loss event before sync readers return the overrun

## Finding

The runtime loss-counter path previously marked `sync.buf_mgmt.overrun_pending`
before appending `RX_STREAM_OVERRUN(FPGA_RX_LOSS)` to RF event history. A
concurrent `sync_rx()` could consume that marker, return, and let the wrapper
poll history before the loss cause had been recorded. This was the same
cross-layer ordering class already fixed for short USB transfers.

## Change

`sync_rx_report_fpga_loss()` now accepts a lock-safe event publisher. It sets
the overrun state and appends the source event while holding the sync buffer
management lock. Readers cannot consume the overrun and return until the
reason is durable in the independent RF event ring. The bladeRF2 callback
only takes the RF event-history lock; it does not take `dev->lock` or re-enter
sync RX. If sync RX is not configured, the event is still published.

## Validation

- `host/misc/run_sync_rx_epoch_traversal_test.sh`: PASS, including a new
  regression that verifies the event publisher runs inside the sync overrun
  fence with the FPGA-loss reason staged.
- `libbladerf_shared` production build: PASS.
- `host/misc/run_rx_transition_policy_test.sh`: PASS.
- `host/misc/run_rx_epoch_metadata_test.sh`: PASS.
- `git diff --check`: PASS.

No hardware result is claimed. This change orders discontinuity reporting; it
does not make samples around an FPGA loss valid or repair analog signal
quality. Timeout and fixed discard remain unable to certify IQ.
