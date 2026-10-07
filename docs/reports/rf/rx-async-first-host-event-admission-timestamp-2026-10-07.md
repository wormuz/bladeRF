# Async RX first-host-data admission timestamp

## Defect

Async RX validated a META buffer using a monotonic timestamp captured at the
admission check, then `RX_FIRST_VALID_HOST_DATA` performed a second clock read
when committing the event. If that second read failed, the buffer could still
be delivered under the first successful deadline check while the durable event
contained an invalid timestamp. A transition waiter could then report timeout
after host IQ had already been admitted.

## Change

The async admission commit now passes its already checked monotonic timestamp
to first-host-data event publication while holding `rx_async_epoch_lock`. The
event timestamp and packet admission proof therefore share one observation.
The existing sync path continues to use its timestamp captured before payload
copy. A missing timestamp still rejects async IQ when first-host-data is a
required event; timeout never validates a packet.

## Validation

- Production `libbladerf_shared` build passed.
- `libbladeRF_test_sync_epoch_traversal` passed with a new regression that
  asserts the first-host-data event contains the exact monotonic admission
  timestamp and satisfies its deadline.
- `run_rx_transition_policy_test.sh` passed.
- `libbladeRF_test_sync_worker_stop` passed.
- `hdl/quartus/qcheck` passed.
- `git diff --check` passed.
- No hardware run was performed for this host-side timestamp linearization.
