# NIOS RFIC safe-point response backpressure

Date: 2026-10-14

## Finding

The xA4 NIOS RFIC status service hook runs from cooperative SPI/delay safe points. Its response path previously called the ordinary UART response publisher, which could poll the response-pending MMIO bit up to 100,000 times when the single response slot was occupied. That made the cooperative hook itself block while waiting for the host to drain a response. This is a concrete bounded-but-blocking path; it does not by itself prove the cause of every observed NIOS no-response incident.

The xA4 platform config allocates `RAM_SPAN=131072`, which enables `BLADERF_NIOS_LIBAD936X`; the affected service-hook path is therefore compiled for xA4. Lower-RAM compatibility builds retain the legacy publisher.

## Source change

The xA4 path now attempts publication with one status read and returns a distinct published, backpressured, stale-session, or error result. The already prepared response remains attached to the packet while backpressured, so retry does not repeat packet side effects. The regular xA4 packet-response path uses the same lifecycle. Request admission remains fenced until the response is queued, and a session change retires the stale prepared response. Publication is retried from later service passes rather than spinning inside the safe point.

The shared policy and source regression are in `command_uart_response_policy.h`, `test_command_uart_response_policy.c`, and `run_command_uart_response_policy_test.sh`. The regression source covers slot classification and prepare-once/retry policy. It has not been compiled or run.

## Remaining verification

The source fix is committed and pushed to `wormuz/bladeRF:master` as `f086fd9a`, `63fc3625`, and `656cf13d`. Review performed so far is source inspection plus `git diff --check`; no build, test, synthesis, image load, or hardware qualification has been performed. The following integration cases remain open:

- response-slot backpressure followed by host drain and eventual retry;
- session reset while a response is prepared, with no stale response publication;
- sticky overflow/error behavior remains fail-closed;
- real packet effects execute exactly once across retries;
- response retry resumes after the RFIC callback returns.

This closes one concrete safe-point blocking defect, not the complete NIOS liveness audit or the overall event-driven release. Continue with the reachable callback/service-gap bound, terminal-result/session correlation, and RX/TX validity propagation before the grouped verification and exact-image qualification gates.
