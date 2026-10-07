# Sticky FPGA RX fault fence before epoch certification

Date: 2026-10-07

## Finding

The micro FPGA exposes `rf_link_status[14]` as a synchronized sticky aggregate of RX-side link faults (speed mismatch, no progress, GPIF timeout, protocol error, FIFO abort). libbladeRF read this word only for verbose diagnostics around RF-link start/stop. `bladerf_rx_transition_wait()` could observe the RX epoch gate become active and publish `RX_EPOCH_VALID` without checking whether the RX data writer had faulted during the handoff.

## Change

Before installing the host timestamp boundary and publishing `RX_EPOCH_VALID`, libbladeRF now reads the RF-link status and checks the sticky RX-fault bit. A read failure or asserted fault fails the transition through the existing abort/invalidation path. An asserted fault publishes a terminal `ERROR` event carrying the raw RF-link status and does not emit `RX_EPOCH_VALID`. The ABI is unchanged, and production builds have no fault-injection behavior.

Added `host/misc/rx_transition_fpga_fault_live.c`, a live xA4 harness for RX1, RX2, and paired RX_X2. In a test build, the existing transition-stall injection variable can set the fault bit at the status-check boundary after the real NIOS status read.

## Verification

- Production `libbladerf_shared` build passed.
- Native `libbladeRF_test_sync_epoch_traversal` and `libbladeRF_test_sync_worker_stop` passed.
- Production xA4 `rx_transition_validity_live` passed: valid epoch/IQ, sync timeout boundary notification, and fail-closed legacy LO/bandwidth/sample-rate changes.
- Test-injection xA4 runs passed for RX1, RX2, and RX_X2. Each reported `ERROR` with the RX-fault bit set, no `RX_EPOCH_VALID`, and sync RX returned zero samples without modifying the sentinel buffer.

The injection changes the host-observed status word after a real NIOS read; it does not electrically trigger an FPGA watchdog/GPIF/FIFO fault. The guard covers the transition handoff. Continuous monitoring of sticky FPGA faults after epoch certification remains open; runtime timestamp discontinuities and sync timeouts continue to use their existing withheld/overrun notifications.
