# RX fault-cause read failure remains visible (2026-10-08)

## Finding

The runtime monitor already revoked the certified RX epoch and emitted
`RX_DATA_INVALIDATED` whenever the FPGA summary status reported an RX fault.
However, when reading the detailed NIOS `RX_FAULT_CAUSES` word failed, the
invalidation event used `error_code=0`. IQ was withheld, but the diagnostic
failure that prevented identifying the FPGA fault was lost in the event stack.

## Change

The monitor now records whether the cause-word read was attempted and passes
its failure through the invalidation event's `error_code`. A successful read
with no cause bits while the summary fault bit is set is marked
`BLADERF_ERR_UNEXPECTED`. Synthetic summary faults that did not attempt the
detail read retain `error_code=0`. The primary invalidation reason and
fail-closed RX behavior are unchanged.

## Verification

- Production `libbladeRF` and `libbladeRF_test_sync_epoch_traversal` rebuilt.
- Native policy regression covers no read, valid cause word, read failure, and
  inconsistent empty cause word.
- `libbladeRF_test_sync_epoch_traversal`: exit 0; timeout diagnostics in its
  output are expected cases in the existing traversal suite.
- `hdl/quartus/qcheck`: clean.
- `git diff --check`: clean.

No hardware fault was injected in this run. The change improves event
diagnostics; it does not change the existing invalidation boundary or make a
timeout/discard establish IQ validity.
