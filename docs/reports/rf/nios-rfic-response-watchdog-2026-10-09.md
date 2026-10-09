# NIOS RFIC response watchdog audit

Date: 2026-10-09

## Finding

RFIC commands are executed synchronously inside the NIOS packet handler. The
handler sends its USB/UART response only after the command returns. The bundled
ADI no-OS `ad9361_check_cal_done()` polls up to 20,000 times and delays 1,200 us
between reads for `REG_CALIBRATION_CTRL`, a nominal 24 s per such calibration,
plus SPI and handler overhead. A Host-to-FPGA/NIOS mode-switch probe remained
blocked for about 55 s before the host probe was interrupted. This is a lower
bound on that wait, not evidence that initialization completed in 55 s.

Previously the RFIC response transfer used a 6 s deadline, shorter than one
bounded AD9361 calibration. A timeout can therefore occur before firmware
returns its explicit result and leaves the NIOS request/response channel
ambiguous. Replaying OUT is unsafe because the command protocol has no
transaction ID; the transport now sends once and remains fail-closed after an
ambiguous IN failure.

## Change

The RFIC response watchdog is now 120 s, above the preserved 55 s blocking
trace. This only changes how long the host waits for a matching response. It
does not certify RFIC state, open an RX epoch, or mark IQ valid. The true
worst-case duration of every synchronous NIOS RFIC command remains unbounded
by current evidence, and the recurring no-response cause remains open. A
progress-capable/asynchronous NIOS command protocol or a proved command
execution bound is still needed before treating NIOS ownership mode as release
qualified.

## Verification

- `libbladerf_shared` builds successfully.
- `libbladeRF_test_nios_transaction` passes. Its fake USB backend verifies the
  configured response deadline is passed to IN, a request is sent only once,
  and an IN timeout makes the channel desynchronized with no later transfer.
- `run_nios_rfpll_batch_test.sh` passes packet, mapping, and queue lifecycle
  tests.
- `git diff --check` passes.
- No hardware request, FPGA reload, USB reset, or RF operation was performed.

The tests verify transport behavior and configuration only; they do not
qualify the 120 s deadline against live NIOS execution or explain the preserved
nonresponse.
