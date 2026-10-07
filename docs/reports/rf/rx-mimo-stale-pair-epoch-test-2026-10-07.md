# Verify stale paired RX1/RX2 data is fenced as one epoch

The event-driven RX epoch is shared by RX1 and RX2, and `BLADERF_RX_X2`
timestamps advance once per paired sample even though META payload counts
interleaved channel samples. Existing sync traversal tests covered contiguous
MIMO packets and invalidation of a certified MIMO parser, but did not put an
old-epoch paired packet directly before a new-epoch paired packet.

Added a regression to `libbladeRF_test_sync_epoch_traversal`: with an RX_X2
layout, enqueue an epoch-7 pair below the certified timestamp boundary followed
by an epoch-8 pair at the boundary. The sync reader must discard the complete
old pair and return samples only from the new pair, with timestamp 3000 and
epoch ID 8. This verifies that one channel cannot leak stale interleaved data
while the other channel has crossed into the new epoch.

Validation:

- `cmake --build host/build --target libbladeRF_test_sync_epoch_traversal -j2` — PASS.
- `host/build/output/libbladeRF_test_sync_epoch_traversal` — PASS. It prints
  two expected 1 ms timeout diagnostics from existing negative-path fixtures.
- `git diff --check` — PASS.

This is host-side regression evidence for paired META traversal. It does not
replace hardware qualification of RX1+RX2 transitions on the xA4. No timeout
or fixed discard establishes IQ validity.
