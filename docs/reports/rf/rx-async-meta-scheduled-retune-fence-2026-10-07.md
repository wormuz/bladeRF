# Block unreported scheduled retunes under the async RX epoch contract

Date: 2026-10-07

## Finding

The bladeRF 2 scheduled RX retune guard checked whether an initialized sync-RX stream had epoch filtering enabled. The async META consumer uses the same RX epoch contract but has no sync filter object, so `bladerf_schedule_retune()` could enqueue a LO change without a host transition-complete event or first-valid timestamp. Samples after that hardware retune could retain the old epoch tag and pass async admission as certified.

## Change

Scheduled RX retunes are now rejected with `BLADERF_ERR_WOULD_BLOCK` while either the event-driven epoch contract is enabled or the sync stream epoch filter is active. The transaction-owned quick-tune substep remains allowed because its FPGA epoch gate is armed and its completion is observed by `bladerf_rx_transition_wait()`. Legacy scheduled retunes remain available before callers opt into epoch protection and for TX.

Added an async live regression that first proves a raw async stream cannot enter the epoch contract, completes a valid transition, then starts an async META stream with no sync stream configured and attempts an ordinary scheduled RX retune. It requires rejection and an unchanged LO.

## Verification

- Full host CMake build passed.
- `host/misc/run_rx_transition_policy_test.sh` passed, including separate async-contract and sync-filter policy cases.
- `host/misc/run_rx_epoch_async_format_live.sh` passed on xA4: the async META stream was active, scheduled retune returned `BLADERF_ERR_WOULD_BLOCK`, and RX LO remained at 1,835,399,998 Hz (2 Hz readback offset from the requested 1,835,400,000 Hz).
- A subsequent event-driven transition had already completed successfully before the async META test, confirming the API continues to support valid retunes through the transaction path.

No timeout or elapsed-time rule establishes IQ validity. Ordinary scheduled retunes stay outside the event-driven contract because they have no completion event or epoch boundary.
