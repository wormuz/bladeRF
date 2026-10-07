# RX transition first-host-data wait — 2026-10-07

## Change

Added opt-in `BLADERF_RF_REQUIRE_FIRST_HOST_DATA` to the RX transition
requirements. It implies `BLADERF_RF_REQUIRE_EPOCH_VALID`, keeps the existing
FPGA epoch validation, and makes `bladerf_rx_transition_wait()` wait for the
first META packet that passed host epoch and timestamp validation.

The wait uses a condition variable signalled when the first host-data event is
recorded. It does not poll or sleep. The transition's monotonic deadline is
shared with RX admission: while this option is active, a candidate first
packet is withheld if no deadline has been installed, if the monotonic clock
fails, or if admission occurs at or after the deadline. Completion also checks
the recorded event timestamp is strictly before the deadline. A late packet
therefore cannot turn a timed-out transition into success.

The event carries the certified epoch's transaction and epoch IDs, and the
first admitted packet timestamp. This option proves first host delivery for
the stream admitted by the current transition path; it does not yet establish
that both RX1 and RX2 have delivered data for a paired RX_X2 capture.

The C API remains opt-in for compatibility. The Python binding exposes the
requirement bit and documents that timeout withholds late first data.

## Validation

- Shared library and `libbladeRF_test_sync_epoch_traversal` build.
- Native sync epoch traversal passes; its deliberate timeout cases emit
  expected buffer-timeout diagnostics.
- RX transition requirement policy tests pass, including strict-before,
  equal-to, after-deadline, missing deadline, and clock-failure cases.
- RX epoch metadata contract test passes.
- Python extension builds against this checkout's public header and shared
  library; RF event notification tests pass (14 tests).
- No attached-board run was performed in this change.

## Remaining scope

This closes the gap between FPGA epoch certification and host delivery for
callers that request it. It does not prove analog settling beyond the RFIC
status events, and it does not implement the planned paired-channel contract
or hardware qualification for RX1+RX2.
