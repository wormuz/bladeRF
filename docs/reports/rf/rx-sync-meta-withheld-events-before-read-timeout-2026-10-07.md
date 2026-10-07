# Publish RX META rejection causes before a blocked sync read expires

The sync META parser already rejected samples from an uncertified epoch,
timestamp discontinuity, or mismatched epoch and retained the rejection reason
for `RX_DATA_WITHHELD`. It appended that event only after `bladerf_sync_rx()`
returned. A large read that drained stale messages and then waited for new data
could therefore leave the wrapper's background event poller unaware of the
reason until the read timeout.

The parser now publishes its first precise withheld reason immediately through
the lock-safe `rx_data_withheld_at` board hook when the current read has not
copied a valid prefix. The callback is safe under sync/ring locks on bladeRF 2
and only appends to the native event history. Reads that already contain a
valid prefix retain their established host-data-before-gap event order and
report after returning that prefix. The later sync-timeout notification remains
separate and never establishes validity.

Validation:

- `libbladeRF_test_sync_epoch_traversal` — PASS. A threaded test drains four
  old-epoch META messages, then blocks waiting for the expected epoch. It
  observes `EPOCH_OR_TIMESTAMP_MISMATCH` before the 300 ms read timeout and
  confirms the read returns zero samples with `BLADERF_ERR_TIMEOUT`; a second
  event reports the timeout itself.
- `host/misc/run_rx_epoch_metadata_test.sh` — PASS.
- `host/misc/run_rx_transition_policy_test.sh` — PASS.
- Python wrapper suite against the rebuilt native library — 23 passed.
- `git diff --check` — PASS.

No elapsed time or fixed discard converts rejected IQ into valid data.
