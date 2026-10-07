# Revoke RX validity when sample interpretation mode changes

Date: 2026-10-07

## Finding

`bladerf_enable_feature()` changed `dev->feature` without participating in the RX epoch contract. On bladeRF 2.x, `BLADERF_FEATURE_OVERSAMPLE` changes the sample-rate reporting/range rules and restricts supported stream formats. Toggling it while an RX epoch was certified could leave consumers using IQ under a different software interpretation while the old epoch remained marked valid.

## Change

Feature changes now invalidate the shared RX certificate before updating `dev->feature`, then release the setter reservation. This applies both when enabling a feature and when resetting to `BLADERF_FEATURE_DEFAULT`. An invalid or unsupported enable request is rejected before invalidation; a failed RX fence leaves the feature unchanged. The new public reason is `BLADERF_RF_INVALIDATE_FEATURE`, exposed in Python as `RF_INVALIDATE_FEATURE` / `feature`.

## Verification

- Full host CMake build passed.
- Native RX shared-clock invalidation test passed, including enable, disable, reason identity, and fail-closed state preservation.
- `run_rx_transition_policy_test.sh` and `libbladeRF_test_sync_epoch_traversal` passed.
- Python extension rebuilt; the RF notification suite passed (12 tests) with unique reason values and `feature` mapping.
- `run_rx_transition_validity_live.sh` passed on xA4 in RX1, RX2, and RX1+RX2 layouts. Each run enabled `OVERSAMPLE`, verified the invalidation reason and zero-sample `WOULD_BLOCK`, reset to `DEFAULT`, verified a second invalidation, and restored IQ only after a new event-driven transition.

Feature toggling does not itself program AD9361 registers. It changes the API's mode for interpreting/configuring sample rates and formats; the subsequent event-driven transition is still required to re-certify RX data after the mode change. No elapsed-time or discard-based validity is introduced.
