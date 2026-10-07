# Fence RX validity around opaque FPGA writes

Date: 2026-10-07

## Finding

The public `bladerf_config_gpio_write()` wrote the full FPGA configuration GPIO register without using the RX invalidation contract. That register includes RX mux and clock-selection controls, so a caller could change the RX sample source while an earlier epoch remained certified. `bladerf_wishbone_master_write()` exposed arbitrary FPGA writes with the same gap: it can address RX admission and metadata state, but the generic library cannot classify the target address as harmless.

## Change

Both public opaque write APIs now revoke the shared RX data-valid epoch before entering the board write and release the setter reservation after the write returns. A failed fence prevents the FPGA write. New reason bits distinguish `config_gpio` from `wishbone`; the Python wrapper exports and names both reasons.

This behavior is intentionally conservative for opaque writes. Typed setters retain their more specific reason flags. On devices without the bladeRF 2 RX invalidation callback, the compatibility helper remains a no-op.

## Verification

- Full host CMake build passed.
- `libbladeRF_test_rx_shared_clock_invalidation` passed with added mock checks that both low-level writes invalidate first, preserve the RX1 shared-certificate scope, complete the reservation, and do not reach the board writer when invalidation fails.
- `libbladeRF_test_sync_epoch_traversal` passed.
- `run_rx_transition_validity_live.sh` passed on xA4 in RX1, RX2, and paired RX1+RX2 layouts. Each mode read config GPIO, wrote the same value back, observed `RX_DATA_INVALIDATED(config_gpio)`, verified sync RX returned no IQ, and restored IQ only after a fresh event-driven transition.
- Python extension rebuilt; `tests/test_rf_event_notifications.py` passed (12 tests), including distinct public values and reason names for every invalidation reason.

Wishbone writes are exercised through the native mock API test rather than by writing arbitrary FPGA addresses on a live board; the latter would risk corrupting hardware state. The public path and fail-closed ordering are covered without mutating live FPGA registers.

## Validity rule

Neither timeout nor elapsed time certifies data. An opaque write leaves the RX certificate revoked until the normal event-driven transition publishes a valid epoch.
