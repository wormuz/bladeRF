# AD9361 initialization SPI fail-open audit

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

A follow-up walk through the initialization call chain found the same issue
in the main setup helpers: AGC configuration (69 SPI writes), parallel-port
configuration (15), TX monitor configuration (8), and the final ENSM wake
from SLEEP. Those helpers could continue after an unsuccessful write, or
publish a requested ENSM state after its SPI operation failed.

## Change

The initialization calibration paths now propagate configuration SPI errors.
Filter tune circuits are disabled after calibration or setup failures, and
cleanup failures are returned when there is no earlier error. TX quadrature
also checks the filter-control/status reads, retries, and phase search; cleanup
attempts to restore RX phase-inversion state, prior RF bandwidth, and TX
synthesizer powerdown state. AGC, parallel-port, and TX-monitor setup now stop
on the first failed SPI write. The ENSM transition checks clock, state,
VCO-calibration, lock-status, and MGC overload-counter operations before
updating the cached state.

## Verification

- `host/misc/run_ad9361_calibration_read_error.sh`: PASS. Injected failures
  into RX/TX BB filter calibration, RX TIA reads, synth charge-pump setup, BB
  DC setup, RF DC setup, TX quadrature setup/status accesses, AGC/parallel
  port/TX monitor configuration, and ENSM wake writes are returned as `-EIO`.
- `cmake --build host/build --target ad936x -j2`: PASS.
- `git diff --check`: PASS.
- NIOS application rebuild was attempted, but this shell has no
  `QUARTUS_ROOTDIR`/NIOS toolchain configured; it is not claimed as verified.
- No FPGA synthesis, image load, or board operation was performed.

This closes the audited initialization-calibration and setup-helper SPI
fail-open paths. A broader audit of every helper reachable from
`ad9361_setup()` is still required before claiming the full initialization
path is fail-closed. It does not explain the preserved NIOS mode-switch
no-response incident or close exact release-image RX1/RX2/RX_X2 qualification
and LTE RF-content acceptance.
