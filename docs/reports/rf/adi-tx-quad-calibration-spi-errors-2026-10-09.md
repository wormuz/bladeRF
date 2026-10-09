# AD9361 TX quadrature calibration SPI errors

Date: 2026-10-09

## Finding

`ad9361_setup()` invokes TX quadrature calibration during RFIC initialization.
The calibration code issued setup writes without checking their return values,
ignored a failed `REG_INVERT_BITS` read, and could continue after the initial
calibration helper failed. In the helper, three setup writes and one or two
calibration-status reads were also unchecked. A transport/register access
failure could therefore be hidden and later code could report calibration
success.

## Change

The TX quadrature path now propagates setup-write, filter-control read,
calibration status-read, retry, and phase-search errors. Cleanup attempts to
restore RX phase-inversion state, the prior RF bandwidth, and TX synthesizer
powerdown state; cleanup errors are returned when no earlier error exists.

## Verification

- `host/misc/run_ad9361_calibration_read_error.sh`: PASS. Injected failures
  into the TX quadrature NCO write, control write, and calibration status read
  are returned as `-EIO`.
- `cmake --build host/build --target ad936x -j2`: PASS.
- `git diff --check`: PASS.
- NIOS application rebuild was attempted, but this shell has no
  `QUARTUS_ROOTDIR`/NIOS toolchain configured; it is not claimed as verified.
- No FPGA synthesis, image load, or board operation was performed.

This closes the identified TX quadrature SPI fail-open only. It does not
explain the preserved NIOS mode-switch no-response incident or close exact
release-image RX1/RX2/RX_X2 qualification and LTE RF-content acceptance.
