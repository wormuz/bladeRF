# Publish sync RX worker overruns immediately

The sync worker detected dropped, reordered, and ring-full RX buffers and set
`overrun_pending`, but the shared RF event history was updated only when a
later `bladerf_sync_rx()` call returned. An event poller could therefore miss
the fault while a large read remained in progress or while no sync read was
active. This delayed the unconditional fault notification at the library and
wrapper boundary.

Added a callback-safe board hook used by the sync RX worker. On the first
overrun in a pending interval, it appends `RX_STREAM_OVERRUN` immediately with
the queue/reorder source bits and current RX epoch identity, without taking
`dev->lock`. The existing sync metadata overrun status remains set. A latch
prevents `sync_rx()` from appending a duplicate when it later consumes that
status; if a backend does not implement the immediate hook, the existing
post-read event path remains the fallback.

Validation:

- Rebuilt `libbladeRF_test_sync_epoch_traversal`, now linked with the real
  `sync_worker.c` — PASS.
- Regression invokes the real rejected-buffer callback with a full ring and
  verifies the overrun hook fires before any sync read.
- Event-history test holds `dev->lock` while publishing and verifies the
  callback safely appends the expected event, epoch ID, and source bits.
- `host/misc/run_rx_epoch_metadata_test.sh` — PASS.
- `host/misc/run_rx_transition_policy_test.sh` — PASS.
- `git diff --check` — PASS.

Overrun is reported as a stream-integrity event; it does not certify IQ.
Timeout and fixed discard remain failure/rejection mechanisms only.
