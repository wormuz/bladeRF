# FPGA reload fail-closed recovery qualification

Date: 2026-10-09

## Recovery behavior

`bladerf2_load_fpga()` no longer assumes a failed backend load leaves the old
image active. It queries `is_fpga_configured()` before attempting RFIC
restoration. If the FPGA is not confirmed configured, the handle moves to
`STATE_FIRMWARE_LOADED`; ordinary RF operations fail state checks until a
fresh image loads. If the image remains configured, the RFIC is restored and
the RX fault monitor is restarted only when it was running before reload.
RFIC or monitor restoration failure also moves the handle to the fail-closed
state.

The injection is controlled by the default-OFF
`ENABLE_TEST_FPGA_LOAD_FAILURE_INJECTION` build option. The test target has
three scenarios:

- `--failed-reload-only`: injects before backend entry, confirms RF tuning
  still works after RFIC/monitor restoration, then performs a real reload.
- `--failed-reload-unconfigured-only`: simulates an unconfigured status,
  confirms RF tuning is refused, then performs a fresh real reload.
- `--failed-backend-reload-only`: enters USB FPGA configuration mode and sends
  `BEGIN_PROG`, transfers a 4096-byte bitstream prefix, then injects an I/O
  error. It verifies that FPGA status is unconfigured, tuning is refused, a
  new known-good image reloads, and close completes.

The test hooks do not exist in the default production library. The regular
build's `ENABLE_TEST_FPGA_LOAD_FAILURE_INJECTION=OFF` was verified and its
`libbladeRF.so` contains neither injection environment variable.

## Hardware results

All tests used xA4 serial `f695006ba84a40daa7b777c6a6eba78` and the exact
runtime-qualified hosted image:

```text
RBF: hostedxA4-2026-10-09_08.19.04/hostedxA4.rbf
SHA-256: afb9b39272e938830714a66358faf5f9c6913a96791f57fb2ccafdd66bed3113
```

1. Pre-backend failure: expected `BLADERF_ERR_UNEXPECTED`; AD9361 recovery
   succeeded; a subsequent RX frequency update and real same-handle reload
   succeeded; close exited 0.
2. Synthetic unconfigured report: the library logged FPGA status 0, refused
   RX tuning in `Firmware Loaded` state, accepted a fresh real image load,
   and closed with exit 0.
3. USB backend partial-transfer boundary: after `BEGIN_PROG` and a successful
   4096-byte transfer, the injected I/O failure left the FPGA unconfigured.
   The library logged status 0 and refused RX tuning. A fresh real load of the
   same hashed hosted RBF then configured the device. On that same handle,
   event-driven RX required PLL lock, ENSM RX, FPGA epoch validity, and
   first-host-data; `sync_rx` returned 4096 samples whose META epoch matched
   the transaction, timestamp was at/after the boundary, and overrun was
   clear. RX disable and close succeeded with exit 0. Full trace:
   `fpga-reload-backend-failure-20261009.log`.

After the backend-boundary recovery, the standard non-injection native RX
event qualification completed three cross-band transitions each in RX1,
RX2, and paired RX_X2. All nine had complete event histories, zero
unrecovered transitions, first-read faults, retries, or stream overruns, and
clean close. P99 was 19.660 ms (RX1), 21.045 ms (RX2), and 21.514 ms
(RX_X2).
Logs: `fpga-reload-recovered-rx1-3-20261009.log`,
`fpga-reload-recovered-rx2-3-20261009.log`, and
`fpga-reload-recovered-rxx2-3-20261009.log`.

All image operations were volatile. No SPI flash write occurred.

## Gate status and limits

The failed-reload release gate is closed for a partial USB transfer that
leaves the FPGA unconfigured: the device failed closed, accepted a fresh
volatile image, and completed the full event/host-data validity path on the
same handle. The one-shot fault is test-only and absent from production.
Failures during RFIC restoration or monitor-thread creation are handled by
keeping the handle in fail-closed `STATE_FIRMWARE_LOADED`; those resource
failure branches were reviewed but not fault-injected. The nine follow-up
transition checks are smoke qualification, not a replacement for the
