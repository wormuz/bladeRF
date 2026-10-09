# Hosted A4 runtime qualification — 2026-10-09

## Exact image and build

The hosted A4 RBF was built and checked by qgate earlier the same day. SHA-256:

```text
afb9b39272e938830714a66358faf5f9c6913a96791f57fb2ccafdd66bed3113
```

It was loaded through `bladerf_load_fpga()` on the connected xA4 using the local rebuilt libbladeRF. The same-handle load and close completed; there was no SPI flash write. The full source build used the standard configuration with `ENABLE_TEST_RX_TRANSITION_STALL_INJECTION=OFF` except during the explicitly labeled injected-fault tests.

## Cross-band RX transition coverage

On the exact hosted RBF, the native `rx_epoch_transition_qualification` harness ran 100 cross-band retunes for each mode: RX1, RX2, and paired RX_X2. All 300 transitions completed with zero unrecovered reads, first-read faults, retries, overruns, incomplete event histories, or close failures. Every result contained the required event trace and valid epoch-tagged host data. P99 transition latency was 21.501 ms (RX1), 20.434 ms (RX2), and 23.311 ms (RX_X2).

After the subsequent fault-event ordering change, the standard non-injection build passed another 10 cross-band transitions in each mode with complete event history, no stream errors, and clean device close. These are separate smoke runs, not added to the 300-transition distribution.

## Runtime invalidation and recovery

The test-only `RUNTIME_FPGA_FAULT` injection matrix passed for RX1, RX2, and RX_X2. In each case:

- the blocking sync read returned `BLADERF_ERR_WOULD_BLOCK` with zero samples;
- the durable `RX_DATA_INVALIDATED` event timestamp was no later than the sync reader completion timestamp;
- the caller's destination buffer was not populated by invalid IQ; and
- a later explicit transition opened a distinct epoch and returned valid samples.

The live test previously compared when an event-polling thread noticed the event with when a reader thread set its completion flag. That scheduler-order check could fail even when the event was already durable. It now compares the event's monotonic timestamp directly with the reader's monotonic completion timestamp. Production code also commits the invalidation event within the sync epoch-generation revoke callback, before releasing the generation lock, so a read cannot observe revocation before the reason is in history.

The runtime fault is a host test injection into the monitor's observed status, not an electrical fault or a physical FPGA FIFO fault. It validates invalidation publication, sync withholding, and recovery across the full software stack on the hosted FPGA image.

## Same-handle reload regression

The existing `libbladeRF_test_fpga_load` gained `--reload-only`, which isolates the host-tuning lifecycle from its separate legacy FPGA-tuning/sample-rate checks. The regression opens one handle, loads the same hosted RBF, changes sample rate, reloads it again on that handle, and closes. With `BLADERF_FORCE_FPGA_A4=1`, it passed. Both monitor lifecycles and the final board/backend close completed. No SPI flash write was performed.

The legacy full test proceeds after its two reloads into `TUNING_MODE_FPGA` and then attempts another sample-rate change; that step returned `BLADERF_ERR_UNEXPECTED` on the hosted image. It is outside this release's host-tuning runtime gate and was not counted as a reload-lifecycle failure. The `--reload-only` path tests the required same-handle reload behavior directly.

## Evidence and commits

Scanner logs:

- `hosted-a4-same-handle-load-close-20261009.log`
- `hosted-a4-rx1-100-crossband-20261009.log`
- `hosted-a4-rx2-100-crossband-20261009.log`
- `hosted-a4-rxx2-100-crossband-20261009.log`
- `hosted-a4-rx1-runtime-fault-retest-20261009.log`
- `hosted-a4-rx2-runtime-fault-20261009.log`
- `hosted-a4-rxx2-runtime-fault-20261009.log`
- `hosted-a4-rx1-production-postfix-smoke-20261009.log`
- `hosted-a4-rx2-production-postfix-smoke-20261009.log`
- `hosted-a4-rxx2-production-postfix-smoke-20261009.log`
- `hosted-a4-double-reload-lifecycle-regression-20261009.log`

The invalidation ordering change and timestamp regression are in bladeRF commit `a2d76a8d`. The reload monitor lifecycle implementation remains in `e4192c57`; the focused test option was added in `a2d76a8d`.
