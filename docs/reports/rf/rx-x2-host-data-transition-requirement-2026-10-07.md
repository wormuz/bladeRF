# RX_X2 host-data transition requirement — 2026-10-07

## Change

Added `BLADERF_RF_REQUIRE_RX_X2_HOST_DATA`. It normalizes to
`BLADERF_RF_REQUIRE_FIRST_HOST_DATA`, `EPOCH_VALID`, and the required RFPLL,
BBPLL, and ENSM confirmations. The waiter succeeds only after a host-validated
RX_X2 META block is recorded for the same transaction/epoch.

The library rejects a request before RF mutation if an RX_X1 sync stream is
configured or an RX_X1 async stream is active. While the requirement is
active, RX_X1 sync configuration and async stream startup return
`BLADERF_ERR_UNSUPPORTED` and append `RX_LAYOUT_UNSUPPORTED`; async packet
admission also has a layout guard. Failed layout attempts do not revoke an
already valid epoch or alter the LO. The async RX_X1 consumer count is guarded
by `dev->lock`, serializing stream start/stop with transition begin.

Python exports the requirement and the new event. The dual-channel
`pybladerf_sweep` path requests RX_X2 host data, performs its first bounded
sync read before waiting, retains that IQ block, then requires the waiter to
return `rx_first_valid_host_data` with `rx_layout == "RX_X2"` before placing
the block on the detector queue. Read errors still retire the RF transaction
through the waiter; timeout or read failure never queues IQ.

For synchronous callers, a data consumer must run before or concurrently with
`bladerf_rx_transition_wait`; the wait does not consume application IQ. The
sweep path uses the read-before-wait sequence and retains that first block.

## Validation

- Production libbladeRF and native sync traversal executable build.
- Native traversal verifies RX_X2 satisfies a paired request, RX_X1 does not
  satisfy it, event layout provenance, and the reason-coded layout rejection.
- RX transition policy tests cover X2 requirement normalization, X1 blocking,
  and layout acceptance; RX epoch metadata test passes.
- Python Cython extension and dual-channel sweep extension build against the
  changed API.
- 24 Python event and sweep-helper tests pass.
- A live harness was added at `host/misc/run_rx_x2_transition_requirement_live.sh`.
  USB enumeration sees xA4 serial `f695006ba84a40daa7b777c6a6eba78`, but
  `bladerf_open()` fails while reading the FPGA version with
  `BLADERF_ERR_TIMEOUT`; `fuser` reports no process holding the USB node.
  Therefore no RF mutation or RX_X2 live qualification was performed.

## Remaining scope

The software contract now rejects the wrong stream layout and requires the
paired host block. RX1-only/RX2-only channel routing, detector parity, RF
sensitivity, physical transport faults, and repeated xA4 RX_X2 hardware
qualification remain open.
