# TX retune gate fail-closed source audit (2026-10-10)

Status: source-only. No build, test, simulation, synthesis, or board operation was performed.

## Defects found

The TX transition monitor revoked host-side TX admission when a retune gate status read failed, when the FPGA reported the gate ERROR state, or when the COMPLETE command returned an error. It did not request an FPGA ABORT in those cases. A status transport error leaves the physical output state unknown; a failed COMPLETE write has an ambiguous outcome because the command may have reached the FPGA. Host-side stream rejection alone therefore did not establish that the FPGA output was muted.

A failed ABORT acknowledgement also left a gap in the recovery contract: the monitor reported status unavailable, but the direct-frequency and scheduled-retune entry points did not consult the pending abort or the hard recovery block. Another RF operation could begin before mute was confirmed.

## Source changes

- Gate status errors, FPGA ERROR, and ambiguous COMPLETE errors now request ABORT through the monitor's common handling path. `nios_tx_retune_gate_abort()` only succeeds after observing FPGA ERROR with the MUTED bit.
- Failed ABORT acknowledgement sets `tx_recovery_blocked`, marks the TX stream faulted, and reports status unavailable.
- Direct TX frequency reconfiguration and scheduled TX retune reject requests while ABORT is pending or recovery is blocked.
- Added test-build-only runtime fault modes for gate status-read failure, COMPLETE failure, and ABORT failure. The live harness checks stream rejection, confirmed recovery after successful abort, and continued blocking when mute acknowledgement fails.
- Fixed the harness dispatch so the ABORT-failure scenario actually exercises its scheduled retune.

## Verification boundary

`git diff --check` passes. The harness and test-only hooks have not been compiled or run. The full RX+TX source audit remains open; no-build gate remains in force until the source sweep is complete. FPGA/NIOS gate behavior and the physical TX mute boundary still require exact-image hardware qualification.
