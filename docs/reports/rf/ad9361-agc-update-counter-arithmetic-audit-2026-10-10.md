# AD9361 AGC update-counter arithmetic audit (2026-10-10)

Status: source-only. No build, test, simulation, synthesis, or hardware operation was performed.

## Defect

`ad9361_gc_update()` computed the AGC gain-update counter with 32-bit unsigned multiplication and subtraction. A zero or too-short configured update interval underflowed when settling overhead was subtracted; a large interval could overflow before the hardware-field clamp. Both cases could silently program a plausible but incorrect counter.

## Source correction

- Added a checked helper that performs the interval/overhead arithmetic in 64 bits, rejects an interval shorter than the settling overhead, and clamps only the final representable counter.
- Preflighted the computed counter before the first AGC SPI write, then reused the precomputed settling delay and counter in the register sequence.
- Added source regression cases for zero, insufficient, valid, and saturating intervals.

## Verification boundary

The new regression is included in the existing SPI-injected AD9361 source harness but has not been compiled or run under the active no-build gate. `git diff --check` is the available validation for this change.
