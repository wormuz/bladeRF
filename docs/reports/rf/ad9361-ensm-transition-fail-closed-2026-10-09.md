# AD9361 ENSM transition failures now propagate on checked paths

Date: 2026-10-09

## Finding

ADI no-OS `ad9361_ensm_force_state()` and `ad9361_ensm_restore_state()` returned
`void`. They could hide an SPI read/write error or a failure to reach the
requested ENSM state. The LO clock-mux path and calibration/bandwidth paths
could therefore continue without an affirmative RX/ALERT state result.

## Change

no-OS commit `dcd92e1e0` adds checked force/restore APIs. They validate the
RFIC pointer, propagate SPI errors, poll the actual ENSM state with a bounded
10 ms deadline, and return `-ETIMEDOUT` if the state does not match. A failed
initial state read clears the saved state so a later restore cannot reuse a
stale value. The LO clock-mux, RFDC calibration, timed RFDC calibration, and
RF bandwidth update paths use the checked APIs and propagate their failures.
The original void entry points remain as compatibility wrappers.

## Verification

- `host/misc/run_ad9361_calibration_read_error.sh`: PASS, including injected
  ENSM status/config read and write errors, unconfirmed-state timeout, and
  confirmed ALERT→RX restoration.
- Host `ad936x` target builds successfully.
- Hosted and sweep NIOS ELFs build with `-Werror`; generated RAM-init files
  match (SHA-256 `52108c936fa59decd019838f2a5258a6c644f44e32da9fba0cea9abcd0bd7930`).
- Hosted Quartus full compile is running with the regenerated NIOS RAM image;
  sweep full compile and final qgate/RBF checks are pending.
- No image was loaded and no hardware qualification was run.

The change closes false-success reporting for the listed checked paths, not
for every legacy void ADI caller. Exact-image RX1/RX2/RX_X2 qualification and
the preserved NIOS no-response incident remain open release gates.
