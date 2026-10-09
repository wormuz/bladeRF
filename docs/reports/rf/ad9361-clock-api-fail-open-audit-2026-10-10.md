# AD9361 clock API failure-propagation audit

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
3. `ad9361_clk_register()` uses the checked recalculation functions through
   unchecked wrappers. `ad9361_register_clocks()` stores every returned
   pointer without checking `NO_OS_IS_ERR()`. Thus an SPI failure while the
   initial clock tree is read can leave zero rates or an encoded error pointer
   in `phy->clks[]` while registration returns success.

The first two paths affect public set/get clock-chain operations. The third
affects initialization and is the highest-priority fix because subsequent
RFIC setup can proceed from an unverified clock tree. This audit does not
claim that any of these paths caused the preserved NIOS no-response or LTE
no-PSS observations.

## Planned fix and regression coverage

- Propagate the clock-chain setter result before running bandwidth updates.
- Add checked clock-rate retrieval for the public clock-chain getter and
  preserve negative SPI errors.
- Make clock registration stop on the first checked recalc failure, free the
  partial tree safely, and return the original error to `ad9361_init()`.
- Extend the existing SPI fault-injection test to fail representative BBPLL,
  RFPLL, and clock-chain reads/writes and assert that no public API reports
  success or a complete clock tree.

Source audit only; no runtime device operation or RF setting change was made.
Implementation is pending the active exact-source Quartus build so its source
identity remains unambiguous.

## Source locations

- Vendored ADI source: `thirdparty/analogdevicesinc/no-OS/drivers/rf-transceiver/ad9361/ad9361_api.c`
- Clock registration and clock-chain APIs:
  `thirdparty/analogdevicesinc/no-OS/drivers/rf-transceiver/ad9361/ad9361.c`
- Current failure-injection harness: `host/misc/test_ad9361_calibration_read_error.c`
