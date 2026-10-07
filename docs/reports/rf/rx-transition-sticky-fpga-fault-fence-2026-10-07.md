# Sticky FPGA RX fault fence at transition and runtime

Date: 2026-10-07

## Finding

The micro FPGA exposes `rf_link_status[14]` as a synchronized sticky aggregate of RX-side link faults (speed mismatch, no progress, GPIF timeout, protocol error, FIFO abort). libbladeRF read this word only for verbose diagnostics around RF-link start/stop. `bladerf_rx_transition_wait()` could observe the RX epoch gate become active and publish `RX_EPOCH_VALID` without checking whether the RX data writer had faulted during the handoff.

## Change

Before installing the host timestamp boundary and publishing `RX_EPOCH_VALID`, libbladeRF now reads the RF-link status and checks the sticky RX-fault bit. A read failure or asserted fault fails the transition through the existing abort/invalidation path. An asserted fault publishes a terminal `ERROR` event carrying the raw RF-link status and does not emit `RX_EPOCH_VALID`. The ABI is unchanged, and production builds have no fault-injection behavior.

The same sticky status is also checked by the existing 100 ms RX integrity
monitor while a certified RX epoch and RF link are active. A runtime read
failure, unsupported status version, or asserted RX-fault bit revokes the
matching certified epoch, closes the FPGA epoch gate, invalidates sync RX,
and publishes reason-coded invalidation/overrun events. The monitor also
checks RFPLL, ENSM, BBPLL, and the FPGA loss-event counter; see
`rx-transition-fenced-watchdog-and-bbpll-2026-10-07.md` and
`rx-fpga-loss-event-notification-2026-10-07.md` for their qualification
evidence.

Added `host/misc/rx_transition_fpga_fault_live.c`, a live xA4 harness for RX1, RX2, and paired RX_X2. In a test build, the existing transition-stall injection variable can set the fault bit at the status-check boundary after the real NIOS status read.

## Verification

- Production `libbladerf_shared` build passed.
- Native `libbladeRF_test_sync_epoch_traversal` and `libbladeRF_test_sync_worker_stop` passed.
- Production xA4 `rx_transition_validity_live` passed: valid epoch/IQ, sync timeout boundary notification, and fail-closed legacy LO/bandwidth/sample-rate changes.
- Test-injection xA4 runs passed for RX1, RX2, and RX_X2. Each reported `ERROR` with the RX-fault bit set, no `RX_EPOCH_VALID`, and sync RX returned zero samples without modifying the sentinel buffer.

The transition test injection changes the host-observed status word after a
real NIOS read; it does not electrically trigger an FPGA watchdog/GPIF/FIFO
fault. The handoff check and runtime monitor both exist. Electrical injection
of a physical GPIF/watchdog/FIFO fault and qualification of which individual
sticky cause bits set under each physical failure remain open hardware-
validation items; the current status word exposes only their sticky aggregate.
