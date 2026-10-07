# RF event history rejects future cursors

## Finding

`bladerf_rf_events_get_since()` accepted any 64-bit `after_sequence`. When a
caller supplied a cursor greater than the event ring's current sequence, the
API returned zero events with `history_complete=true` and preserved the
fabricated cursor. Subsequent RF invalidation events remained hidden until the
sequence caught up, with no history-loss notification.

## Change

The history API now validates the cursor against the current tail while
holding the event-history mutex. A future cursor returns `BLADERF_ERR_INVAL`,
sets `history_complete=false`, returns no events, and resets `next_sequence`
to the actual tail. Python's existing incomplete-history path therefore emits
`rf_event_history_lost` and resynchronizes on the next poll. Valid old cursors
still report overwritten history as incomplete; undersized output buffers
retain their existing `BLADERF_ERR_MEM` behavior.

A policy regression covers current, older, and future cursors, including
`UINT64_MAX`. `qcheck` verifies the production API uses this validation and
resynchronizes to the tail.

## Validation

- `libbladerf_shared` production build passed.
- `libbladeRF_test_sync_epoch_traversal` passed.
- RX transition policy and RX epoch metadata tests passed.
- `hdl/quartus/qcheck` passed.
- Python RF event notification tests: 14 passed with the rebuilt libbladeRF
  loaded globally.
- Scanner stream, paired stream, and radio surface tests: 39 passed against
  the rebuilt libbladeRF.
- `git diff --check` passed.

No hardware test was needed for the cursor policy. This change only improves
notification and recovery when a consumer supplies an invalid event cursor;
it does not alter IQ validity rules.
