# RX sync first-host event and invalidation linearization — 2026-10-07

## Finding

The sync META parser checked its RX epoch generation before returning IQ, then
released the generation lock before publishing `RX_FIRST_VALID_HOST_DATA`.
An invalidation could advance the generation and revoke the device-side epoch
certificate in that interval. The parser had already accepted and copied the
IQ, but the event callback could observe the revoked certificate and omit the
host-data event. This was a software event/provenance race; it did not allow an
unvalidated epoch to pass the parser's generation check.

## Change

- Sync RX now commits its first-host-data callback after the final generation
  check and before releasing the generation lock.
- Added callback-capable sync invalidation/revocation helpers. They advance the
  sync generation and revoke the bladeRF2 device certificate in one lock
  order: sync generation lock, then device RX epoch lock.
- Transition failure, ordinary RX reconfiguration, runtime FPGA fault, and
  transition begin now use the coordinated revocation path where they revoke
  both sync and device admission state.
- Added a concurrent traversal regression which pauses the host-data event
  commit and proves invalidation cannot overtake it. If invalidation wins the
  generation lock first, sync RX retains the existing fail-closed behavior.
- Added a qcheck rule requiring both operations to remain under the shared
  generation lock.

## Validation

- Production `libbladerf_shared` build: PASS.
- `libbladeRF_test_sync_epoch_traversal`: PASS, including the new race case;
  repeated 25 times: 25/25 PASS.
- `libbladeRF_test_sync_worker_stop`: PASS.
- `libbladeRF_test_rx_shared_clock_invalidation`: PASS.
- `hdl/quartus/qcheck`: clean.
- `git diff --check`: clean.

This validates software ordering and fail-closed delivery under concurrency.
It does not qualify RF analog settling, electrical USB faults, or hardware
behavior on the xA4 board.
