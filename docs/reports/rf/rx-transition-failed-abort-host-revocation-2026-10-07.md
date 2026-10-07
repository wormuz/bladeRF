# Failed RX transition revokes host admission before FPGA ABORT

Audit found that `_abort_transition()` attempted the NIOS/FPGA ABORT and
reported `RX_EPOCH_ABORT_FAILED` if that command failed, but did not revoke
the already certified host epoch. This matters after `RX_EPOCH_VALID`, such
as a transition waiting for required first host META data: if the wait timed
out and ABORT failed, the FPGA could remain active while sync/async host paths
still trusted that epoch.

Transition failure cleanup now first revokes `rf_transition_epoch_certified`
and the async timestamp cursor under the admission lock, signals any first
host-data waiter, and invalidates sync RX delivery generation/parser state.
Only then does it issue FPGA ABORT. An ABORT failure remains visible as
`RX_EPOCH_ABORT_FAILED`; the host fence no longer depends on that control
transaction succeeding. The original transition status is still returned.

The native sync traversal regression starts with a certified epoch and queued
matching META, runs the host revoke used by failure cleanup, and verifies
`WOULD_BLOCK`, zero actual samples, and an unchanged caller sentinel. The live
xA4 harness now includes `REQUIRE_FIRST_HOST_DATA` timeout with injected NIOS
ABORT failure, checks IQ withholding, then requires explicit transition
recovery.

Validation: production libbladeRF build, test-injection libbladeRF build,
native `libbladeRF_test_sync_epoch_traversal` in both builds, live-harness
`-Werror` syntax check, and `git diff --check` pass. The live hardware scenario
was added but not executed in this run. Neither timeout nor discard creates
IQ validity.
