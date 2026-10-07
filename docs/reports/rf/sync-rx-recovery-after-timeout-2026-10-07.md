# Sync RX recovery after failed transition — 2026-10-07

## Regression coverage

Extended the production-linked sync epoch traversal test with one end-to-end
parser/ring sequence: a partial old-epoch META buffer is revoked, the old read
times out with no IQ, a fresh explicit epoch is activated, and the first
post-activation USB buffer contains both stale and current-epoch META messages.
The sync consumer must drop the stale message and return samples only from the
new epoch at or after its timestamp boundary.

This complements the existing positive-timeout tests, which require a new
event-driven transition before data recovery. It does not turn timeout or
discard into validity: the accepted samples carry the fresh epoch ID and meet
the installed FPGA timestamp boundary.

## Validation

- `libbladeRF_test_sync_epoch_traversal` rebuilt and exited successfully.
- Its deliberate sync timeout diagnostics are expected; assertions verify
  zero samples on the failed read and correct epoch/timestamp on recovery.
- `git diff --check` passes.
- The live positive-timeout RX1 harness rebuilt the test-enabled library, but
  `bladerf_open()` failed while reading FPGA version (`BLADERF_ERR_TIMEOUT`),
  before any RF configuration or retune. No hardware recovery result is
  claimed in this run.

## Remaining scope

This proves parser/ring recovery for the represented stale-plus-current META
sequence. It does not reproduce the physical async worker/USB queue stall on
the board. Hardware recovery and positive-timeout qualification remain
necessary when the xA4 opens successfully.
