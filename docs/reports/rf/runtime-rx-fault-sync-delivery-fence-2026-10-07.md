# Revoke sync RX delivery before runtime-fault notification — 2026-10-07

## Finding

The runtime FPGA-fault monitor revoked async admission before publishing
`RX_DATA_INVALIDATED`, but it did not revoke the sync delivery generation
until after the NIOS epoch-ABORT control request. A sync read entering during
that request could still see the old parser latch and consume queued samples
under the prior certified epoch.

## Change

Runtime-fault handling now revokes sync delivery generation before appending
the invalidation event or issuing FPGA ABORT. `sync_rx()` compares the current
generation with the generation certified by the last successful epoch at read
entry, in addition to its existing final generation check. Thus reads that
start after revocation fail closed even before parser/ring invalidation can
acquire `sync->lock`. Epoch activation updates the certified generation and
clears the parser latch in one generation-locked commit. The normal parser and
ring invalidation still follows the event/ABORT boundary.

Neither the event nor the generation change establishes new IQ validity; only
a later successful epoch activation can certify a generation.

## Validation

- Production `libbladerf_shared` and `libbladeRF_test_sync_epoch_traversal`
  build.
- Native regression revokes delivery before parser invalidation, verifies a
  queued old-epoch buffer returns `BLADERF_ERR_WOULD_BLOCK` with zero samples
  and an unchanged destination, then confirms a new explicit epoch recovers
  from a mixed stale/current META buffer.
- Native sync epoch traversal exits successfully; expected timeout-path
  diagnostics remain present.
- `git diff --check` passes.
- No hardware result is claimed. The current xA4 open attempt times out while
  reading FPGA version before RF configuration.
