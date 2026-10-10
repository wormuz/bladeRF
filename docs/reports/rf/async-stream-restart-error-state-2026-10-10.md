# Async stream restart error-state audit (2026-10-10)

Status: source-only. No build or test was run.

## Defect

`async_run_stream()` reused `stream->error_code` across backend runs. The
libusb backend is intentionally restartable by the synchronous worker after a
terminal transfer error, but a later clean run returned the previous run's
error and TX could publish that stale value again as a transport failure.

## Change

Clear `error_code` under the stream lock at the beginning of every run. The
libusb backend also resets per-run completion, timeout-deduplication, and RX
terminal-notification latches when it restarts. It also clears the TX cancel
deadline: reusing an expired deadline can cancel a new run's in-flight OUT
transfers immediately, in the middle of an FX3 DMA buffer. Without resetting
the RX terminal latch, a second run could suppress its own terminal
invalid-data notification after an earlier run had already reported one. The
sync epoch traversal source regression now runs a TX stream with a stream
error, follows it with a backend failure, then verifies a clean third run
returns success and does not publish another transport error.

## Verification boundary

Source and whitespace review only. The regression is not executed while the
RX+TX source-audit gate remains closed.
