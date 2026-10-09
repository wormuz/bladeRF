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

## Adjacent fail-open paths found in the expanded sweep

A broader scan of public AD9361 control APIs found additional cases in the
same class:

1. ENSM mode reads ignored one or both SPI errors; the setter ignored the
   `ad9361_set_ensm_mode()` result and updated FDD-independent cached state
   before the hardware transition succeeded.
2. RX gain-control mode always returned success and updated its software mode
   cache even if the ADI AGC configuration failed. The lower-level routine
   also ignored RX re-enable failure after the register write.
3. RSSI read parsed an uninitialized register buffer after a failed SPI burst.
4. RX/TX FIR coefficient readers ignored writes selecting the FIR and each
   coefficient address. Data-read failures also returned before clearing the
   FIR clock-start bit.
5. MCS ignored delay-register reads/writes, ENSM transitions, and each MCS
   step result. A failed step could therefore be reported as success.
6. Channel-mode reset ignored reset, soft-reset, setup, and post-setup errors;
   its clock tree also assigned TX reference rate twice instead of updating
   RX reference rate on the second assignment.
7. Fastlock profile loading ignored SPI writes and populated the profile cache
   even when programming failed. The per-word helper also combined results
   with bitwise OR rather than stopping at the first failed write.
8. RX gain get/set paths ignored status reads, and the table getters could
   return fabricated gain after a failed SPI read or index an invalid table
   entry. Split-table reads also failed to restore the indirect table address
   on later errors.
9. FIR coefficient loading ignored setup and per-tap SPI writes, then restored
   filter state without checking that restoration succeeded. The debug FIR
   verifier had the same unchecked access pattern.
10. Temperature/AuxADC getters ignored power/read/restore failures; foreign
    fastlock exit ignored every recovery write before clearing cached profile
    state; and clock/data-delay helpers reported success with incomplete
    hardware or cached values.
11. TX mute cached failed attenuation reads as real settings and combined two
    restore-operation statuses. TX attenuation read also treated every channel
    number other than one as TX2.

These adjacent paths are now fail-closed: errors are returned, temporary FIR
state is restored on read failure, software cache updates follow successful
hardware operations, and MCS attempts to restore both ENSM states after a
mid-sequence failure. Fastlock metadata is committed only after the complete
profile transfer and programming-clock stop succeed. Channel-mode changes now
propagate reset, setup, and post-setup errors. RX gain output is committed only
after the complete checked table read; invalid gain indexes are rejected.
Temperature/AuxADC and clock-delay paths preserve read errors and report failed
cleanup writes.

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
  registration, both BIST writes, ENSM getters, RX gain get/set, AGC/RSSI,
  temperature/AuxADC, RX/TX FIR readers and loader, fastlock load/exit, clock
  delay reads/writes, and TX mute. The test passes.
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
