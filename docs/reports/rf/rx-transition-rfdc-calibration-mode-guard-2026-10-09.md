# Reject event-driven RX transitions in NIOS tuning mode

Date: 2026-10-09

## Finding

The event-driven RX transition implementation is host-controller-specific. Its wait path reads AD9361 status through `board_data->phy`; switching to `BLADERF_TUNING_MODE_FPGA` deinitializes the host RFIC controller and clears that pointer, while frequency operations move to NIOS. The public begin API documented host-mode tuning but did not enforce it. A transition begun in FPGA/NIOS mode could later dereference the null host PHY while observing PLL/ENSM state. Explicit RFDC calibration had the same null-PHY hazard.

## Change

Transition begin now checks tuning-mode ownership under the device lock. Every event-driven transition request in FPGA/NIOS mode returns `BLADERF_ERR_UNSUPPORTED` before allocating a transaction, invalidating the current RX epoch, or touching the RFIC. Legacy `bladerf_set_frequency()` in NIOS mode is unchanged. The public API now states the mode restriction.

This is a fail-closed boundary, not NIOS event-chain support. NIOS-owned tuning still needs a separate transition protocol that reports its RFIC and calibration completion into the shared FPGA epoch contract before it can be qualified for event-driven RX.

## Verification

- Built production `libbladeRF.so` with all test fault-injection options OFF.
- Rebuilt and ran `libbladeRF_test_sync_epoch_traversal`; exit status 0, including mode-policy assertions.
- Verified the production shared library has SONAME `libbladeRF.so.2`, no RPATH/RUNPATH, and no test-injection strings.
- `git diff --check` passed.

No live NIOS-mode hardware transition was attempted; the guard returns before changing mode-specific RF state. Hardware qualification of NIOS event reporting remains open.
