# RX trigger arm clock-domain synchronization

Date: 2026-10-08

## Finding

The sweep xA4 Quartus Design Assistant identified
`nios_system_rx_trigger_ctl:rx_trigger_ctl|data_out[0]` as a D103 asynchronous
clock-domain crossing. This NIOS PIO output directly drove the RX trigger's
`armed` input in the AD9361 RX sample-clock domain. The TX trigger already
used a `reset_synchronizer` for its arm input; RX did not.

## Change

`rx.vhd` now synchronizes trigger arm into `rx_clock` using the same
fail-safe-level scheme as TX: the PIO's disarmed low level asynchronously
asserts disarm, and activation is released only after the synchronizer clock
stages. Fire/master retain their existing trigger-line control semantics.

Added `rx_trigger_arm_cdc_tb.vhd`, checking initial disarm, two-clock delayed
activation, and immediate asynchronous disarm. The existing `qsim` skipped
`reset_synchronizer.vhd` because its source filter also matched that filename;
the filter now skips only the generic synchronizer whose output attribute must
be stripped for GHDL. The bench now finishes cleanly with `std.env.finish`.

## Validation

- `qsim rx_trigger_arm_cdc_tb`: 1 case passed.
- `qcheck`: clean.
- Full Quartus Prime 25.1 Standard xA4 sweep compile: 0 errors; Fitter 0
  errors/26 warnings; Assembler 0 errors; Timing Analyzer 0 errors/36 warnings.
- The target RX trigger PIO is absent from the D103 critical list. Aggregate
  D103 structures decreased from 9 to 8; D101 structures decreased from 721
  to 720. D103's remaining listed nodes are FIFO fault-status paths, and the
  project-wide qgate still fails on 42 critical-warning records. No warnings
  were waived.
- Worst setup slack: +0.183 ns. Worst hold slack: +0.186 ns. No negative
  slack.
- `sweep.rbf` SHA-256:
  `5851e11a15f6c503fb6239a253dd185404331b73968b9d9a0950acdb453db885`.
- The image was not loaded to hardware. No RF or RX epoch behavior claim is
  made from this CDC change.

The full compile log is retained in the ignored Quartus work directory at
`hdl/quartus/work/adr0207-rx-cause-full-xa4/logs/adr0207-rx-trigger-arm-cdc-full-20261007.log`.
