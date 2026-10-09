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
12. Fastlock profile save packed many direct SPI reads into bytes without
    checking any of them, so an incomplete profile could be silently saved.
13. External-LO control ignored its initial MCS-state read, overwrote the
    first synth-control write result with a later write, and changed cached
    synth power state before the register sequence succeeded.
14. BIST loopback ignored observe/config reads and helper return values,
    accepted an invalid mode only after beginning a register read, and
    committed the cached mode before the final hardware write.

These adjacent paths are now fail-closed: errors are returned, temporary FIR
state is restored on read failure, software cache updates follow successful
hardware operations, and MCS attempts to restore both ENSM states after a
mid-sequence failure. Fastlock metadata is committed only after the complete
profile transfer and programming-clock stop succeed. Channel-mode changes now
propagate reset, setup, and post-setup errors. RX gain output is committed only
after the complete checked table read; invalid gain indexes are rejected.
Temperature/AuxADC and clock-delay paths preserve read errors and report failed
cleanup writes. Fastlock save and BIST loopback return the first failed
operation, while external-LO control updates its synth-power cache only after
the whole register sequence succeeds.

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
  delay reads/writes, TX mute, fastlock save, external-LO control, and BIST
  loopback. The test passes.
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

## RX/TX and IIO/AXI follow-up sweep (2026-10-10)

The follow-up pass extended the same fail-closed review through the shared RX
and TX paths, IIO setters, digital interface tuning, and the AXI ADC helper
used by the AD9361 chain. It found additional false-success and stale-cache
cases:

- IIO numeric setters accepted malformed values as zero, silently narrowed
  RF frequency to 32 bits, or accepted partial fixed-point/GPO/fastlock input.
  Gain parsing also accepted non-finite values, and several callbacks dropped
  lower-level errors or returned an error for successful BB DC tracking.
- Combined RX/TX FIR enables applied one direction before a second-direction
  failure and did not use the existing transactional TRX API. RSSI table cache
  rollback could also claim old tables after new tables had reached hardware
  but ENSM restore failed.
- The AXI ADC layer ignored register read/write failures in PN selection and
  monitoring, clock-frequency reads, calibration scale/phase/bias access, and
  delay calibration. Calibration getters could modify output buffers before
  successful reads; active-channel mask cache was committed before the writes
  and then accidentally stored the shifted mask.
- Digital-tune state preservation called a checked ENSM helper that was
  private to another translation unit. The compile pass exposed that interface
  mismatch; the helper is now declared explicitly in the AD9361 header.

Those paths now validate input and ranges, propagate the first SPI/AXI error,
stage output/cache changes until success, use combined RX/TX FIR control, and
keep the requested active-channel mask intact. The TX FIR fault-injection test
now targets the actual TX filter configuration register. `axi_adc_pn_mon()`
retains its existing `-1` PN-mismatch result so current no-OS applications
remain compatible, while transport errors pass through unchanged.

Validation after this pass:

- `host/misc/run_ad9361_calibration_read_error.sh`: PASS.
- `cmake --build host/build --target ad936x -j2`: PASS.
- `cmake --build host/build -j2`: PASS.
- `hdl/quartus/qgate qrun-final-audit.log`: PASS, including timing, fitted
  CDC/skew evidence, and classified Design Assistant findings.
- `hdl/quartus/qcheck`: PASS.
- Full bladeRF-micro A4 seed-1 sweep build: PASS, 21:49 total. Fitter,
  Timing Analyzer, Assembler, and Design Assistant reported zero errors;
  setup slack is +0.095 ns and hold slack +0.112 ns. RBF SHA-256:
  `5c1a077173d2f1bcee002d367f295f4e3483c890ba47877bfb9f1fe2c2e05566`.
  The generated image remains unloaded; exact-image hardware qualification
  and the preserved NIOS no-response investigation remain open.

No claim is made that these defects caused the NIOS no-response or LTE no-PSS
observations. The image was built from the current dirty source tree and must
be reproduced from the final committed tree before release packaging.
