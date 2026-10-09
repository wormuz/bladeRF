# Guard explicit RFDC calibration in NIOS tuning mode

Date: 2026-10-09

## Finding

`bladerf_rx_transition_wait()` performs explicit `RX_RFDC_CAL_DONE` through the host ADI PHY pointer (`board_data->phy`). Switching a bladeRF 2 device to `BLADERF_TUNING_MODE_FPGA` deinitializes the host RFIC controller and sets that pointer to `NULL`; frequency writes then go through NIOS. Before this fix, a transition requesting explicit RFDC calibration could pass the null pointer into `ad9361_do_calib_timeout()`, whose implementation dereferences it while disabling AD9361 tracking. The caller could therefore crash instead of receiving an API error.

## Change

Transition begin now checks the selected tuning mode while holding the device lock. If FPGA/NIOS mode is active and explicit host RFDC calibration is requested, it returns `BLADERF_ERR_UNSUPPORTED` before allocating a transaction, invalidating the current RX epoch, or touching the RFIC. Ordinary NIOS-owned transitions remain available; NIOS retains its own first-tune / ≥100 MHz RFDC policy and the event-driven wait observes its NIOS command response before subsequent RF-state checks. The public API documents the mode restriction.

## Verification

- Rebuilt the affected libbladeRF shared library and `libbladeRF_test_sync_epoch_traversal` from the configured CMake tree.
- Native sync/epoch traversal test passed, including policy checks for host explicit-calibration support, NIOS explicit-calibration rejection, and normal NIOS epoch requests.
- `git diff --check` passed.

A connected-hardware NIOS-mode transition run is still required before qualifying that full path for release; this fix only removes an unsafe unsupported combination and does not claim NIOS-mode hardware qualification.
