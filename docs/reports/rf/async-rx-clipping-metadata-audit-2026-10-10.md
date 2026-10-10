# Async RX callback metadata reset audit — 2026-10-10

## Finding

`async_rx_process_buffer()` cleared the callback-scoped timestamp, flags,
status, sample count, and RX epoch fields before classifying each USB transfer,
but did not clear the later-added `rx_clipping_flags` field or the reserved
bytes. After a valid sample-META transfer or a caller mutation, a following
short, stale-epoch, or otherwise withheld transfer could invoke the callback
with `samples == NULL` while retaining stale callback-scoped metadata.

## Change

The async RX path now resets `rx_clipping_flags` and all reserved bytes with
the other per-transfer fields. The `test_sync_epoch_traversal` regression
source seeds nonzero clipping/reserved fields, asserts valid callbacks receive
the current clipping bits and zeroed reserved bytes, mutates a reserved byte
inside the callback, and asserts later event-only callbacks receive zero.

## Verification status

`git diff --check` passes. The regression source was added but was not built or
run because the active release task requires completing the full RX1/RX2/TX
source defect sweep before any build, test, simulation, or synthesis.
