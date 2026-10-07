# RX transition wait returns its own terminal event

Date: 2026-10-07

## Finding

`bladerf_rx_transition_wait()` copied `rf_transition_last_event` when it
finished. That field is shared with runtime RX overrun and fault notifications,
which are appended independently and commonly use `transaction_id = 0`. A
runtime notification arriving after `RX_EPOCH_VALID` but before wait cleanup
could therefore replace the transition result. The call could report success
with an unrelated final event and transaction identity.

## Change

Completion now selects the newest retained event whose `transaction_id`
matches the waiting transition. Runtime events cannot replace it. The failure
cleanup path uses the same selection rule. If no event for the transaction
remains in history, the call returns an explicit unexpected error result
rather than returning another event as if it belonged to this transition.

## Verification

- Added unit coverage for a runtime overrun appended after a terminal epoch
  event, a wrapped event ring, and a missing transaction.
- `host/misc/run_rx_transition_policy_test.sh` passed.
- `libbladerf_shared` rebuilt successfully.
- Native sync epoch traversal and shared-clock invalidation tests passed before
  this change; the selector change does not modify stream-buffer behavior.
