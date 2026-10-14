# NIOS RFIC safe-point response backpressure

Date: 2026-10-14

## Finding

The xA4 NIOS RFIC status service hook runs from cooperative SPI/delay safe points. Its response path previously called the ordinary UART response publisher, which could poll the response-pending MMIO bit up to 100,000 times when the single response slot was occupied. That made the cooperative hook itself block while waiting for the host to drain a response. This is a concrete bounded-but-blocking path; it does not by itself prove the cause of every observed NIOS no-response incident.

The xA4 platform config allocates `RAM_SPAN=131072`, which enables `BLADERF_NIOS_LIBAD936X`; the affected service-hook path is therefore compiled for xA4. Lower-RAM compatibility builds retain the legacy publisher.

## Source change

The xA4 path now attempts publication with one status read and returns a distinct published, backpressured, stale-session, or error result. The already prepared response remains attached to the packet while backpressured, so retry does not repeat packet side effects. The regular xA4 packet-response path uses the same lifecycle. Request admission remains fenced until the response is queued, and a session change retires the stale prepared response. Publication is retried from later service passes rather than spinning inside the safe point.

The shared policy and source regression are in `command_uart_response_policy.h`, `test_command_uart_response_policy.c`, and `run_command_uart_response_policy_test.sh`. The regression source covers slot classification and prepare-once/retry policy. It has not been compiled or run.

## Follow-up: retry after the last safe point

Reviewing every call site found that the safe-point pump could retain a prepared response on backpressure, but `run_deferred_packet_work()` did not retry that response when the callback returned. If the response slot became free after the final SPI/delay safe point, delivery depended on another safe point that might never occur. The comment claiming that the main loop retried was stronger than the implementation.

The NIOS main-loop deferred-work pump now retries only an already-prepared response after each worker pass, including its early-return ownership paths. It performs one nonblocking publication attempt per pass; it never prepares or executes the request again. Session retirement, fail-closed UART error handling, and ISR admission behavior share the same response lifecycle. The policy regression source now covers eligibility for this post-callback main-loop retry. This closes the source-visible retry liveness gap, pending compilation and integration/hardware verification.

## Remaining verification

The initial nonblocking source fix is committed and pushed to `wormuz/bladeRF:master` as `f086fd9a`, `63fc3625`, and `656cf13d`. The post-callback retry follow-up is a separate source change. Review performed so far is source inspection plus `git diff --check`; no build, test, synthesis, image load, or hardware qualification has been performed. The following integration cases remain open:

- response-slot backpressure followed by host drain and eventual retry;
- session reset while a response is prepared, with no stale response publication;
- sticky overflow/error behavior remains fail-closed;
- real packet effects execute exactly once across retries;
- the compiled main-loop retry is exercised after the final safe point and callback return.

This closes one concrete safe-point blocking defect, not the complete NIOS liveness audit or the overall event-driven release. Continue with the reachable callback/service-gap bound, terminal-result/session correlation, and RX/TX validity propagation before the grouped verification and exact-image qualification gates.
