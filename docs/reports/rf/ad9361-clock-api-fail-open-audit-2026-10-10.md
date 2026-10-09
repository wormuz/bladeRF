# AD9361 clock/BIST failure-propagation audit and fix

Date: 2026-10-10

## Newly identified fail-open paths

The focused audit found three remaining error-propagation gaps in the ADI
clock API:

1. `ad9361_set_rx_sampling_freq()` and `ad9361_set_tx_sampling_freq()` call
   `ad9361_set_trx_clock_chain()` without checking its return value, then
   return only the result of bandwidth update. The clock-chain setter does
   return SPI/clock programming errors, but these two public APIs can hide
   them and report success if the later bandwidth update succeeds.
2. `ad9361_get_trx_clock_chain()` reads each component through
   `clk_get_rate()`. That compatibility helper converts checked SPI/readback
   errors to rate zero, while `get_trx_clock_chain()` returns success without
   validating the component rates. Callers can receive a partially zeroed
   clock plan as valid.
3. `ad9361_clk_register()` used unchecked recalculation wrappers.
   `ad9361_register_clocks()` stored the results without checking errors, so
   a failed initial clock-tree read could leave zero rates and still return
   success.
4. `ad9361_bist_tone()` ignored the first of its two SPI writes, updated its
   cached mode/configuration before either write completed, and used the
   unchecked rate helper for injected tone modes. The second successful write
   could hide a failed first write and report a configuration the hardware
   did not accept.

These paths affected public set/get clock-chain operations, RFIC
initialization, and BIST configuration. This audit does not claim they caused
the preserved NIOS no-response or LTE no-PSS observations.

## Implemented fix and regression coverage

- Added `clk_get_rate_checked()` while preserving `clk_get_rate()` as the
  legacy zero-on-error compatibility wrapper.
- RX/TX sample-rate getters preserve the underlying clock SPI error; setters
  return clock-chain failures before running bandwidth updates.
- Clock-tree registration returns the first checked recalculation error and
  frees all partial clock allocations before returning to `ad9361_init()`.
- BIST now checks sample-clock readback and both register writes; cached BIST
  state changes only after both writes succeed.
- Expanded `host/misc/test_ad9361_calibration_read_error.c` to inject failures
  into sample-rate setters/getters, clock-chain reads, initial clock
  registration, and both BIST writes. The test passes.
- `cmake --build host/build --target ad936x -j2` passes and both repository
  `git diff --check` runs are clean.

No runtime device operation or RF setting change was made. The no-OS fix is
committed as `analogdevicesinc/no-OS` commit `bedcc7236`; the bladeRF test and
submodule pointer update are committed separately. A fresh Quartus/NIOS build
is required before this fix can be included in an FPGA release image.

## Source locations

- Vendored ADI source: `thirdparty/analogdevicesinc/no-OS/drivers/rf-transceiver/ad9361/ad9361_api.c`
- Clock registration and clock-chain APIs:
  `thirdparty/analogdevicesinc/no-OS/drivers/rf-transceiver/ad9361/ad9361.c`
- Current failure-injection harness: `host/misc/test_ad9361_calibration_read_error.c`
