# AD9361 initialization calibration SPI errors

Date: 2026-10-09

## Finding

`ad9361_setup()` invokes several calibrations during RFIC initialization.
TX quadrature calibration issued setup writes without checking their return
values, ignored a failed `REG_INVERT_BITS` read, and could continue after its
initial calibration helper failed. The nested helper also skipped checks on
three setup writes and one or two status reads. The same fail-open pattern was
present in RX/TX analog-filter calibration, RX TIA calibration, synth charge
pump calibration, BB DC calibration, and RF DC calibration. A failed SPI
operation could be hidden and a later status read could make the overall
procedure appear successful.

## Change

The initialization calibration paths now propagate configuration SPI errors.
Filter tune circuits are disabled after calibration or setup failures, and
cleanup failures are returned when there is no earlier error. TX quadrature
also checks the filter-control/status reads, retries, and phase search; cleanup
attempts to restore RX phase-inversion state, prior RF bandwidth, and TX
synthesizer powerdown state.

## Verification

- `host/misc/run_ad9361_calibration_read_error.sh`: PASS. Injected failures
  into RX/TX BB filter calibration, RX TIA reads, synth charge-pump setup, BB
  DC setup, RF DC setup, and TX quadrature setup/status accesses are returned
  as `-EIO`.
- `cmake --build host/build --target ad936x -j2`: PASS.
- `git diff --check`: PASS.
- NIOS application rebuild was attempted, but this shell has no
  `QUARTUS_ROOTDIR`/NIOS toolchain configured; it is not claimed as verified.
- No FPGA synthesis, image load, or board operation was performed.

This closes the identified calibration-setup SPI fail-open paths, not every
unchecked SPI access elsewhere in ADI no-OS. It does not explain the preserved
NIOS mode-switch no-response incident or close exact release-image RX1/RX2/
RX_X2 qualification and LTE RF-content acceptance.
