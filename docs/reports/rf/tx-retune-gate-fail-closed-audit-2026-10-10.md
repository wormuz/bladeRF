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

## Async TX admission revocation during USB-capacity wait

The USB backend performs final TX admission checks after waiting for a free
transfer and after the application callback supplies a buffer, including the
initial buffer set. If RF configuration invalidated TX at one of those points,
it used to set/return `BLADERF_ERR_FPGA_OP`; the generic async stream teardown
then classified that as a transport failure and emitted a second, misleading
TX transport event. Those gate closures now use `BLADERF_ERR_WOULD_BLOCK`, and
the async stream runner excludes that retryable admission result from transport
fault publication. Synchronous TX retains and defers queued buffers on the same
status, while the board RF event carries the actual invalidation reason.
Source-only; regression and hardware qualification remain pending.

## Cypress backend parity follow-up

A subsequent backend-by-backend grep found that the Cypress path still
returned `BLADERF_ERR_FPGA_OP` from its initial-buffer, post-transfer, and
post-capacity-wait TX admission checks. Those gate closures could therefore
be reported by `async_run_stream()` as TX transport failures, unlike the
libusb path. All three now return `BLADERF_ERR_WOULD_BLOCK`; the common native
stream regression also checks that a retryable TX gate result creates no
transport notification. Source-only; no build or test has run.

The shared `async_submit_stream_buffer()` admission checks had the same
permanent `BLADERF_ERR_FPGA_OP` result before and after its stream-start wait.
They now return `BLADERF_ERR_WOULD_BLOCK`, matching both USB backends. The
native regression covers admission already closed and revocation between the
initial and final admission checks, and requires that no payload reaches the backend
and no transport event is emitted. Source-only; not built or run.

## TX link-certificate admission follow-up

The sync and async TX admission checks previously required the stream, USB,
FPGA abort, pending-retune, and frequency-model latches to be healthy, but did
not require a confirmed current FPGA TX-link epoch. This allowed host buffers
to be accepted before first TX enable or after the TX link had stopped. TX
admission now additionally requires `tx_async_link_certified`, established
only by an acknowledged new TX epoch, validated status recovery, or a fully
validated TX retune-gate resume. Transport
fault, stream fault, shared RX/TX START, and confirmed TX stop revoke it. The
shared START and stop paths publish `TX_EPOCH_INVALID` so wrappers observe the
same fence. A policy regression source covers the uncertified case. Source-only;
no build or test has run.

## Follow-up: START acknowledgement must include active directions

A further RX+TX startup trace found that `announce_rf_link_epoch()` accepted a
new per-direction ACK/current bit and a stable fault snapshot, but did not
require the corresponding FPGA `link_active` status bit. The HDL exports
`link_active` independently from epoch-current; the signals cross into
`sys_clock` through separate synchronizers. Host could therefore publish
`TX_LINK_VALID` and reopen producer admission while the FPGA still reported
the TX link inactive. Shared RX/TX START had the same gap for the already
active direction, and RX START could return before its active status was
visible.

The START policy now requires the active bit for every direction whose epoch
ACK is required. It continues polling while the ACK has arrived but the active
state has not, and fails under the existing watchdog if the link never becomes
active. Focused policy assertions cover missing TX active, missing RX active,
and the fully active shared epoch. These source regressions have not been run;
no build, simulation, synthesis, or hardware operation was performed.

## Follow-up: retire an ambiguous START during module-enable rollback

The module-enable rollback path tracked whether the RF-link START request was
attempted and whether its response succeeded. It sent the matching STOP only
when START had a successful response. A NIOS/USB response error can occur
after firmware applied the toggle, leaving an active FPGA epoch behind even
though RFIC rollback succeeded and the API returned failure.

Rollback now issues one STOP whenever START was attempted and both RF
directions are down after rollback, regardless of whether the START response
arrived. If either direction remains active, STOP is withheld. Added policy
assertions for ambiguous START, no START, and a still-active RX or TX direction.
Source-only; regression has not been run and no build/device operation occurred.

## Follow-up: disable must expose FPGA link-stop failure

The disable path called `end_rf_link_epoch()` as a best-effort void helper.
Even when NIOS failed to acknowledge STOP, RX/TX disable returned success; TX
had already emitted `TX_STREAM_STOPPED` without a subsequent status-unavailable
event. The helper now returns the NIOS status. Protocol-v2 STOP failures are
returned from module disable, and TX publishes `TX_STATUS_UNAVAILABLE` and
remains recovery-blocked. The documented legacy `BLADERF_ERR_FPGA_OP` response
is still treated as unsupported-gateware compatibility; other NIOS/transport
errors are actionable.

The same source pass caught `st` being scoped inside the status-snapshot block
but referenced after that block when publishing `TX_EPOCH_INVALID`. Moved it
to the enclosing disable scope. Policy regression source distinguishes v2 STOP
failure from the legacy unsupported response. No build/test was run under the
source-audit gate; `git diff --check` is the only validation.

## Follow-up: status-read failure is not legacy proof

The first STOP fix treated `FPGA_OP` as legacy whenever the status read had not
confirmed protocol v2. That also covered a timeout or other failed status read,
which could hide a real STOP failure. Compatibility now applies only when the
status target explicitly returns `BLADERF_ERR_FPGA_OP` or a successful status
read reports a non-v2 protocol. Unknown status remains fail-closed. Added policy
source cases for confirmed legacy, status-read-unknown, timeout, and success;
unbuilt/unrun under the source-audit gate.

## Follow-up: unsupported TX gain-mode request has no stream side effects

The generic TX gain-mode path was initially treated as a missing TX fence.
Source tracing showed that both bladeRF2 gain-mode callbacks had returned
success without changing RFIC state for TX, and every board reports zero TX
gain modes.
Adding a fence would therefore have stopped/revoked a live TX stream for a
no-op. The API now validates the channel and returns
`BLADERF_ERR_UNSUPPORTED` for TX before RX/TX invalidation or board dispatch.
This also avoids a misleading successful result. The earlier fence-only
implementation was removed. A follow-up API-boundary pass validates the
gain-mode enum before invalidation, and both bladeRF2 RFIC callbacks now return
`BLADERF_ERR_UNSUPPORTED` for TX. Source-only; no build/test under the audit
gate.

## Follow-up: validate gain-stage and correction inputs before fencing

The generic gain-stage setter could revoke RX/TX validity before the board
rejected an unknown stage name, and correction setters did the same for an
unknown correction enum. They now validate the public channel first, resolve
the gain-stage name through the board's read-only range query, and validate
correction enum membership before invalidation. Numeric gain remains clamped
by the board implementation. Source-only; no build/test under the audit gate.

## Follow-up: reject invalid generic RF control enums before stream fencing

Generic setters for tuning mode, loopback, RX mux, and feature selection could
revoke RX/TX validity before a board callback rejected an invalid enum. They
now validate enum membership first; loopback support is checked against the
board's advertised mode list. Unsupported-but-valid board/mode combinations
still reach the board callback after the normal fence. Source-only; no
build/test under the audit gate.

## Follow-up: TX gain-calibration policy must revoke TX validity

`bladerf_enable_gain_calibration()` can update the TX gain target through
`set_gain_locked()` when a calibration table is loaded. Its prior implementation
fenced RX only, allowing a live TX certificate to survive the attenuation
change. The setter now invokes the common TX fence before changing the policy
or physical gain. `gain_applied` is initialized false so a fence failure exits
without reading an indeterminate result flag or changing the policy. No
build/test was run under the source-audit gate.

## Follow-up: loading TX calibration must fence before table replacement

`bladerf_load_gain_calibration()` invalidated RX and reserved that transition
before replacing a calibration table and applying the resulting TX gain. For a
TX channel it lacked the corresponding TX fence. It now calls the shared TX
configuration invalidation under `dev->lock` after RX invalidation and before
table loading, so a fence failure exits without publishing the new table.
No build/test was run under the source-audit gate.

## Follow-up: trigger controls must invalidate the affected RX or TX stream

The public trigger arm path revoked RX validity even for TX, while TX trigger
arm could alter the FPGA TX gate without revoking the TX certificate. Trigger
fire had no RX or TX invalidation at all, so triggering could change which
samples were admitted or when RF output began while the prior certificate
remained visible. Trigger arm/fire now preflight channel, signal, and role;
then revoke the corresponding RX or TX validity before touching hardware.
Raw trigger-register writes also preflight channel and signal before fencing.
Source-only; no build/test under the active no-build audit gate.

The generic controls audit found several deterministic unsupported requests
that were checked only after RX invalidation: FPGA tuning without its FPGA
capability, RFIC BIST loopback under FPGA tuning, VCTCXO taming without the
advertised capability, and SC8 sync formats on non-bladeRF2 boards. These
capability/mode checks now run before invalidation. `bladerf_sync_config()`
also rejects unknown layout/format enums, X2 layouts on a one-channel board,
and 8-bit/packet-meta formats unsupported by the current firmware/FPGA before
touching stream state.
Regression source was added; no build/test under the active source gate.

Stream-lifecycle review then found that `sync_config()` changed shared FPGA
format GPIOs while fencing only RX layouts. Timestamp, packet, 8-bit, and
packed-sample modes are shared state, so TX layout configuration could alter
RX interpretation and RX layout configuration could alter the TX datapath.
The generic wrapper now revokes RX and TX validity for every sync format
reconfiguration and exits before board mutation if either fence fails.
Regression source covers both layouts and TX-fence failure; it remains
unbuilt/unrun under the active gate.

The same shared-format path is used by the asynchronous `bladerf_stream()`
API, which had no RX invalidation at all. Generic stream start now revokes RX
validity for either layout, while the bladeRF2 callback fences TX under the
device lock immediately before applying format GPIOs. Regression source
covers RX/TX start, malformed layout, unsupported X2 layout, and board-start
failure; no build/test.

`bladerf_init_stream()` also inspected `*stream` and an uninitialized sample
rate after `init_stream()` failed, and could divide by a zero transfer timeout.
It now nulls the output before initialization, stops on errors or a successful
null result, and guards rate diagnostics against missing rates and zero
timeout using floating-point arithmetic. Regression source covers init error
and successful-null results; no build/test.

The direct RX/TX FIR setters also accepted arbitrary enum values through to
the host RFIC controller. An invalid value was rejected only after RX/TX
invalidation, and the controller's defensive default unlocked a mutex that the
caller would unlock again on return. Both public setters now validate every
supported FIR enum and the low-sample-rate compatibility rule before acquiring
invalidation/fence state, then recheck the rate after reservation to cover a
concurrent setter. Source-only; no build/test under the active gate.

The same sweep found reset and bootloader handoff only revoked RX validity.
The first TX-fence implementation reused the ordinary configuration fence,
which refuses work while TX recovery is blocked and could therefore prevent
reset from serving as an escape path. Reset and bootloader handoff now use a
dedicated shutdown invalidation: it revokes host admission,
publishes TX configuration invalidation, attempts to mute an active TX gate,
and permits teardown to proceed even if the gate response is unavailable.
The board-level reset callback no longer re-enters the ordinary config fence
after the generic wrapper has performed this shutdown invalidation.
FPGA reload retains the ordinary recoverable TX config fence because its board
callback can reject the reload while an RF direction is enabled.
Source-only; no build/test under the active no-build audit gate.
