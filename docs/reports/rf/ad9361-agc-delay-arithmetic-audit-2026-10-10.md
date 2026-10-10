# AD9361 AGC delay arithmetic audit — 2026-10-10

## Finding

`ad9361_gc_update()` calculated eLNA settling, attack, peak-overload, and
fast-energy delays in 32-bit arithmetic before clamping them to 5-bit fields.
The public platform-data field `elna_settling_delay_ns` is an unconstrained
`uint32_t`, so large values could wrap and program a small delay instead of a
saturated value. A nonzero RF clock below 500 Hz also made the attack-delay
divisor zero. Separately, rounded clock-ratio arithmetic could overflow at
extreme values, and the measurement-duration validation occurred after several
SPI writes, allowing a failed update to leave partial AGC state.

## Change

The delay intermediates and rounded clock ratio now use `uint64_t`, with each
hardware field saturated before narrowing. The RF clock is rejected below
1000 Hz, which keeps the smallest attack-delay divisor nonzero. Measurement
duration and its divider are calculated and validated before the first AGC
register write. `ad9361_gc_setup()` now runs the same timing preflight using
the requested modes before mutating cached modes or programming any of its
registers, so invalid clock/interval inputs cannot leave a partial setup. The
internal update/setup entry points also reject missing state before
dereferencing SPI, platform-data, or clock objects. Setup validates the
documented 16–800 mV LMT overload threshold range before programming; values
below 16 previously underflowed during `(threshold / 16) - 1` and clamped to
the maximum register code.

Regression source in `host/misc/test_ad9361_calibration_read_error.c` covers
invalid AGC setup timing, missing state, out-of-range LMT thresholds, and a
499 Hz clock with no SPI writes or cached-mode mutation, plus `UINT32_MAX`
eLNA/fast-energy delays that must saturate all corresponding fields. The
existing test's write-count baseline was also corrected; its RX ADC no-write
check now uses a separate read-count baseline.

## Validation status

Source review and `git diff --check` only. Per the active release instruction,
no tests, build, simulation, FPGA synthesis, or device operation has been run.
