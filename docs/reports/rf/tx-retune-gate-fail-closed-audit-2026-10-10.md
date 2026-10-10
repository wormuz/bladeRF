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

## Follow-up: shared TX RF-configuration entry point

A follow-up call-graph pass found that direct LO and scheduled-retune guards did not cover the shared `bladerf2_tx_rf_config_change_begin()` path used by bandwidth, gain, and other TX RF setters. After an unconfirmed mute, those setters could still enter configuration. The shared helper now rejects both `tx_recovery_blocked` and `tx_gate_abort_pending`; the abort-failure live scenario also checks that a TX bandwidth setter returns `BLADERF_ERR_WOULD_BLOCK`. This follow-up is source-only and not built or run.

## Follow-up: generic TX gain controls

The generic `bladerf_set_gain()` and `bladerf_set_gain_stage()` APIs also changed AD9361 TX attenuation without entering the board's TX configuration fence. They now invalidate TX before the RFIC write. The abort-failure live scenario checks `bladerf_set_gain()` returns `BLADERF_ERR_WOULD_BLOCK`. TX gain-mode setting remains as the existing no-op behavior and does not fence. Source-only; no compile or runtime test.

The generic gain callback is invoked only after taking `dev->lock`, matching the board callback's serialization contract. If fencing fails, both `set_gain` and `set_gain_stage` release the mutex before returning.

The live harness now has a normal-operation `RUNTIME_TX_GAIN_SETTER` scenario: establish TX frequency validity, change gain, require `TX_CONFIGURATION_INVALID` and sync-TX rejection, then recover with a confirmed retune. It is registered in the runner and remains uncompiled/unrun under the source-audit gate.

## Shared TX-start/RX-setter reservation race

The RX invalidation path deliberately skips the hardware RX ABORT when TX starts with no active RX direction. It also skipped `rf_transition_setter_active`, leaving a window before TX enable in which an RX setter could reserve and mutate the shared RF state; TX enable could then clear that setter's reservation on return. The no-RX case now still reserves the setter interval while skipping only the unnecessary RX ABORT. `test_rx_transition_policy.c` covers TX with no RX (reserve, no abort), TX with RX (reserve and abort), and RX invalidation (reserve and abort). Source-only; test not compiled/run.
