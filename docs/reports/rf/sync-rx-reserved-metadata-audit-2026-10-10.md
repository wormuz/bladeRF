# Synchronous RX reserved metadata audit — 2026-10-10

## Finding

`sync_rx()` cleared its documented output metadata fields on each META read,
but left `bladerf_metadata.reserved[29]` untouched. Reusing a metadata object
could therefore preserve caller data in an output field that the API reserves
for future use and documents as zero-initialized.

## Change

META sync RX now zeros the reserved bytes with the other output fields. The
`test_sync_epoch_traversal` receive helper seeds those bytes before each read
and asserts the returned structure contains zeros.

## Verification status

`git diff --check` passes. No test/build was run while the requested source
defect sweep still gates verification.
