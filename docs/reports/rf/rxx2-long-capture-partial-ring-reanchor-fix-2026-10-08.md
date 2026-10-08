# RX_X2 long-capture rearm: partial-ring re-anchor fix — 2026-10-08

## Root cause

The 250 ms closed-epoch reproduction produced two rejected-marker runs in the
96-slot RX META ring. After consuming one valid full slot, `sync_rx()` skipped
12 rejected slots, then 83 more. At the resulting consumer position, slot 53
was `EMPTY`, had `buffer_dropped == false`, and retained old sequence 629;
the worker's `expected_seq` was 642 and the live expected transfer was in
flight at slot 66. The consumer only re-anchored after skipping exactly all 96
ring slots in one pass. It therefore waited on stale slot 53 while the producer
filled the ring. The live trace then showed ring-full at sequence 697, 64 stale
buffers dropped, and timestamp loss.

The fix keeps the existing full-ring re-anchor and adds the missing partial
lap case: after rejected-slot skipping, if the current consumer slot is
`EMPTY` with a sequence different from `expected_seq`, the consumer searches
for and re-anchors to the live expected sequence. A `FULL` slot is preserved;
the fix cannot jump over a valid unread head.

## Code and regression

`sync_worker.h` now contains the locked ring policy helper and
`sync.c` applies it after every rejected-marker run. Native traversal includes
three cases: retain a valid FULL head, recover from the observed 95/96 partial
lap, and retain the previous complete-ring recovery. The RX_X2 qualification
harness now accepts `RX_FIRST_VALID_HOST_DATA` as a valid terminal result when
the caller requested paired host data; previously it incorrectly required
`RX_EPOCH_VALID` as the terminal event and rejected otherwise valid paired
traces.

## Verification

Native `libbladeRF_test_sync_epoch_traversal` and
`libbladeRF_test_sync_worker_stop` pass. `git diff --check` passes.

On xA4 at 23.04 MS/s, 3,456,000 samples per lane (150 ms), 32,768-sample
stream buffers, cross-band hops, and 250 ms after capture close:

| Layout | Cycles | Read faults / retries / overruns | Teardown |
|---|---:|---:|---|
| RX_X2 | 100 | 0 / 0 / 0 | clean close |
| RX1 | 100 | 0 / 0 / 0 | clean close |
| RX2 | 100 | 0 / 0 / 0 | one run hung after `FINAL_DEVICE_CLOSE_BEGIN`; a fresh 10-cycle run and one-cycle debug run closed cleanly |

RX_X2 transition P99 was 33.305 ms; RX1 was 37.676 ms. RX2 100-cycle data
qualification reported 28.415 ms P99 before its final close hang; its
follow-up 10-cycle run reported 28.415 ms P99 with clean close. The stuck
process was sleeping in futex waits with one libusb event thread polling; ptrace
was denied, so the exact intermittent teardown cause remains open. Its
qualification output is preserved but is not counted as a clean 100-cycle
teardown result.

Raw traces:

```text
rxx2-long-capture-100x-250ms-reanchor-fix-2026-10-08.log
rx1-long-capture-100x-250ms-reanchor-fix-2026-10-08.log
rx2-long-capture-100x-250ms-reanchor-fix-2026-10-08.log
rx2-long-capture-10x-close-check-2026-10-08.log
rx2-long-capture-close-debug-2026-10-08.log
```

RX_X2 also passed 10/10 captures with a 1,000 ms close-pause and clean final
close (`rxx2-long-capture-10x-1000ms-2026-10-08.log`).

Follow-up parser tracing found a second sequence-marker defect. When a ring
slot received a new in-flight sequence, an old `buffer_dropped` marker could
survive until `sync_rx()` and make it skip that live transfer. It now clears
the marker when either normal or rejected callback logic assigns a new
sequence. Epoch discard also rebases `expected_seq` and `cons_i` to the oldest
still-in-flight transfer and clears stale reorder state. Native regression
covers slot reissue and sequence rebase. The parser now logs detailed
epoch/timestamp/cursor context for a true discontinuity at debug level.

Before the marker fix, LTE traces showed a same-epoch timestamp jump after
skipping 10 dropped markers: expected timestamp 1,614,087, received 1,777,607;
the paired capture returned 2,681,728 / 6,912,000 interleaved samples and
failed closed. After the fix, five debug and twenty ordinary 947.5→1835 MHz
RX_X2 runs had zero overruns, short reads, or timestamp discontinuities. The
LTE cell was confirmed in all five debug runs and nineteen of twenty ordinary
runs; one ordinary run had no PSS at 1835 MHz. This closes the reproduced host
ring sequence skip in the test campaign but does not close overall LTE/analog
qualification. The scanner report and raw traces are in
`/home/bonho/projects/sdr-scanner/docs/reports/rf/lte-rxx2-immediate-epoch-close-2026-10-08.md`.
RX2 100-cycle final-close remains a separate open teardown issue.
