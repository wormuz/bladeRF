# GPIF timestamp bundle capture and latest A4 fit

## Change

The FX3 GPIF FSM used to compute `tx_timestamp + 32` before registering the
value in `current.tx_ts_plus32`. That put a carry chain between the held
timestamp bundle and the first destination capture register. The FSM now
captures the raw 64-bit timestamp in `current.tx_timestamp`; the `+32`
comparison is evaluated after that destination register. The SDC and post-fit
binding both end at the raw capture register.

The post-fit report shows this exact bundle at +4.939 ns worst-case max-skew
slack. Across all 168 constrained paths, worst-case slack is +3.867 ns and
zero paths violate max skew.

## Verification

- Quartus Prime Standard 25.1 full A4 sweep flow, seed 5, completed in 15:18.
- RBF SHA-256: `67bfc65bc8fd2c7a856c246b82d041e998ac49637ed91c62209b7fb4427c458f`.
- Post-fit timing report: `gpif-timestamp-raw-capture-seed5-2026-10-08.log`.
- `qcheck` and all 24 GHDL benches pass on the current source tree.
- `report_sweep_setup.tcl` now reproduces the worst 0C setup paths. This fit
  has two setup violations in the RX sample-clock domain: worst slack
  -0.144 ns (also -0.024 ns at 85C), from `fifo_writer.meta_write` into the
  RX META DCFIFO write pointer.
- Design Assistant still reports 725 High findings: D101=724 and C105=1.
  These are not waived. The timing and Design Assistant release gates remain
  red, so this RBF is not accepted for release.

The seed-5 fit started before a later source-only guard change in
`fifo_writer.vhd`; its RBF does not contain that guard. The current source
passes `sample_stream_tb`, which exercises the single-stream bounds check.
A new full fit is still required for the exact final source. No image was
loaded onto hardware.
