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
in AGC configuration (69 SPI writes), parallel-port configuration (15), TX
monitor configuration (8), external LNA and mixer Gm table programming,
RX ADC setup reads, tracking setup, TX attenuation, fastlock preparation,
BBPLL programming, and the final ENSM wake from SLEEP. Some helpers continued
after an unsuccessful write; RX ADC setup converted negative read errors to
unsigned register values; other paths could return success after partial
programming.

The same propagation gap extended through the no-OS clock framework:
`clk_set_rate()` ignored failures from BBPLL, RFPLL, and clock-divider setters,
then continued to refresh its clock cache. RFPLL tuning also ignored the
results of external-band callbacks and logged a failed TX quadrature retune
calibration while still reporting the LO change as successful.

The public RX/TX LO getters also returned success unconditionally. Their
clock-rate readback path discarded SPI errors while reading synthesizer
registers, so a transition consumer could receive an invalid frequency value
with a success status.

## Change

The initialization calibration paths now propagate configuration SPI errors.
Filter tune circuits are disabled after calibration or setup failures, and
cleanup failures are returned when there is no earlier error. TX quadrature
also checks the filter-control/status reads, retries, and phase search; cleanup
attempts to restore RX phase-inversion state, prior RF bandwidth, and TX
synthesizer powerdown state. AGC, parallel-port, and TX-monitor setup now stop
on the first failed SPI write. RX ADC setup validates all three calibration-
register reads. Tracking, external LNA, mixer Gm table, TX attenuation, and
fastlock setup now propagate SPI failures. BBPLL setup stops on the first
failed write; clock-chain FIR enable writes and RFPLL fastlock/VCO-control
operations propagate status. `clk_set_rate()` now returns errors from BBPLL,
RFPLL, and divider setters before refreshing the clock cache. RFPLL tuning
returns external-band and TX quadrature calibration errors, and updates its
last-calibrated frequency only after successful calibration. The ENSM
transition checks clock, state, VCO-calibration, lock-status, and MGC
overload-counter operations before updating the cached state.
Checked RFPLL readback now propagates direct-register and fastlock-address SPI
errors, and the public RX/TX LO getters return those failures instead of
reporting a frequency success.

## Verification

- `host/misc/run_ad9361_calibration_read_error.sh`: PASS. Injected failures
  into RX/TX BB filter calibration, RX TIA reads, synth charge-pump setup, BB
  DC setup, RF DC setup, TX quadrature setup/status accesses, AGC/parallel
  port/TX monitor configuration, external LNA, mixer Gm table, RX ADC reads,
  tracking, TX attenuation, fastlock, BBPLL, ENSM mode, ENSM wake, and the
real `clk_set_rate()` BBPLL and public RX LO readback error paths are returned
as `-EIO`.
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
