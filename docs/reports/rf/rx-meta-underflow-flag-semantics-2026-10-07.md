# RX META underflow flag semantics

Date: 2026-10-07

## Finding

`BLADERF_META_FLAG_RX_HW_UNDERFLOW` is not an RX sample-loss indicator. In the FPGA GPIF state machine, the sticky `underrun` bit is set in `SAMPLE_WRITE_IGNORE` after a TX-side write/underflow condition, cleared while RX samples are read, and copied into the status word of RX META output. The public header previously described it only as a generic device sample-buffer underflow, which could be mistaken for evidence that the RX samples in that metadata message were discontinuous.

The sync parser returns this bit in `bladerf_metadata.status`; it does not use the bit as a certificate for RX continuity. The event-driven async RX path admits samples only after checking the RX epoch tag, first-valid timestamp, and per-message timestamp continuity. A timestamp discontinuity withholds the transfer and publishes the RX overrun/withheld events. The TX underrun flag therefore must not invalidate RX IQ or establish its validity.

## Change

Clarified the public API comment for `BLADERF_META_FLAG_RX_HW_UNDERFLOW`: it is a TX sample-buffer underrun latch mirrored into RX metadata, and RX continuity is determined from RX timestamps and RX discontinuity reporting. The ABI and flag value are unchanged.

## Verification

- `cmake --build host/build --target libbladerf_shared libbladeRF_test_sync_epoch_traversal -j2` passed.
- `host/build/output/libbladeRF_test_sync_epoch_traversal` passed (expected timeout-path diagnostic lines are emitted by its negative-path cases).
- `host/build/output/libbladeRF_test_sync_worker_stop` passed.
- Source trace inspected in `hdl/fpga/platforms/common/bladerf/vhdl/fx3_gpif.vhd` (`SAMPLE_WRITE_IGNORE`, `SAMPLE_READ`, and RX_META status output) and `host/libraries/libbladeRF/src/streaming/sync.c` (status propagation).

No FPGA image or hardware behavior changed in this documentation correction.
