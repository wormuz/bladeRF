# NIOS RFIC initialization blocking audit

Date: 2026-10-10

## Finding

The observed NIOS no-response cannot yet be called a processor or USB deadlock.
The Host-to-FPGA tuning-mode request enters `_rfic_cmd_wr_init()` and calls
`_rfic_initialize()` inline. That path calls `ad9361_init()` and then performs
per-direction filter, sample-rate, and LO setup before returning. The NIOS
packet loop cannot service a second request or the INIT response while this
call is active. The queue used for RFIC writes does not make INIT asynchronous:
its worker also executes each write handler inline.

ADI's `ad9361_check_cal_done()` polls 20,000 times. For calibration-control
registers each unsuccessful poll delays 1,200 us, giving a nominal timeout of
24 seconds for one calibration. `ad9361_setup()` invokes several calibration
families, including RX/TX baseband tune, BB DC, RF DC, TX quadrature, and RSSI
gain-step (unless configured to skip it). Further frequency and sample-rate
setup can invoke clock/VCO waits. A multi-tens-of-seconds INIT is therefore
consistent with the source's finite waits; this is a plausible explanation
for a long unanswered command, not proof that it explains the captured 55 s.

The 2026-10-09 incident was interrupted before INIT returned. Its subsequent
250 ms version-byte request timed out, but that probe did not distinguish a
still-running INIT from a wedged NIOS. It recorded no NIOS PC, calibration
register state, or stage transition during the wait. The current
`init_stage` status is held in NIOS state and can only be queried after NIOS
resumes packet handling, so it cannot diagnose the in-flight no-response.
The existing 120 s libbladeRF response watchdog is appropriately a failure
deadline, not evidence of RF or IQ validity.

## Next diagnostic change

Before changing calibration policy or blaming a hardware wait, add an
independent progress trace for INIT: timestamp entry/exit around each named
ADI setup/calibration operation, plus the last completed stage and current
operation in a host-readable record that remains available while the command
handler is busy. Then reproduce once while preserving the live NIOS/USB state
and correlate the last stage with a direct SPI/clock observation. A trace that
depends on a second NIOS command is insufficient. Do not infer a hard hang
from the 250 ms one-shot probe.

## Evidence

- Incident record: this repository's `docs/reports/rf/nios-mode-switch-no-response-2026-10-09.md`.
- Source: `hdl/fpga/platforms/common/bladerf/software/bladeRF_nios/src/devices_rfic_cmds.c`,
  `devices_rfic.c`, and the vendored ADI `ad9361.c`.
- No hardware state was changed during this source audit.
