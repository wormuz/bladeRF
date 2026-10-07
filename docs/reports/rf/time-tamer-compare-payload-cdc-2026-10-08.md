# Time-tamer compare payload CDC — 2026-10-08

## Change

`time_tamer` previously let `hold_time` change after `intr_set` while a
request/acknowledge crossing was still in flight. The timer could therefore
compare or load a value different from the value software accepted with the
command. It now snapshots `hold_time` into `compare_payload` when accepting
`intr_set`, and holds that payload through destination capture. The past-time
decision and `compare_time` load use the snapshot. The per-instance SDC bounds
now run from `compare_payload` to `compare_time` for both RX and TX timers.

The persistent `time_tamer_tb` regression arms `0x500`, writes `0x600` before
the destination capture, and verifies the interrupt still occurs in
`[0x500, 0x600)`. The test is reachable through `hdl/quartus/qsim`, and its
normal completion reports PASS. `qsim` now has a 30-second default simulation
timeout controlled by `QSIM_TIMEOUT_SECONDS`; this keeps a stuck test from
holding an entire `--all` run forever.

## Verification and remaining failures

- `qsim time_tamer_tb`: PASS; observed firing timestamp `0x502` after the
  software register had been changed to `0x600`.
- `qsim sticky_reduce_tb` and `qsim rx_fault_causes_cdc_tb`: PASS.
- `qcheck`: PASS.
- Quartus 25.1 A4 sweep seed 5 completed fit, timing analysis, assembly, and
  Design Assistant. The fitted timing summary has positive setup and hold
  slack; worst reported hold slack is +0.019 ns in the 0C corner.
- The updated payload SDC matched both `rx_tamer` and `tx_tamer`.
- `qgate` correctly rejects this image: Design Assistant reports 721 D101 and
  one C105 High finding; `report_max_skew` reports five violations, worst
  slack -1.476 ns. The worst reported path is the timestamp handshake into
  `fx3_gpif.current.tx_ts_plus32` (`source_holding[5]` to `tx_ts_plus32[5]`).
  This is a separate bundled-data path and is the next CDC investigation.
- The first full-flow invocation also exposed a cleanup error: the sweep
  max-skew branch closes the project, then unconditional final cleanup closes
  it again. `build.tcl` now tracks project-open state and `qcheck` guards that
  behavior. The full flow has not yet been rerun after this Tcl-only fix.
- `qsim --all` is not green: `dwell_summary_equiv_tb` fails elaboration,
  `dwell_summary_tb` has three assertions, and `fifo_writer_armed_tb` does
  not terminate. The new timeout reports the hang rather than blocking.

No FPGA image was loaded. This verifies the compare-payload CDC protocol and
static fit behavior; it does not qualify RF settling, host IQ validity, or
RX1/RX2 operation.
