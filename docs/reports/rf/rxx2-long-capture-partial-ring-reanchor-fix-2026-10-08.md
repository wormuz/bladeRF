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

This closes the observed stale-EMPTY/partial-rejected-ring reproduction. It
does not close the full long-pause release gate: subsequent LTE sweeps exposed
an intermittent parser discontinuity at the next epoch. Some 947.5→1835 MHz
RX_X2 runs complete and confirm PCI 85; others publish `RX_STREAM_OVERRUN` and
return only 32,704 of 6,912,000 interleaved requested samples. The fail-closed
wrapper rejects those captures. The newer parser/event traces and immediate
finite-capture close are recorded in the scanner repo at
`docs/reports/rf/lte-rxx2-immediate-epoch-close-2026-10-08.md`; the intermittent
discontinuity remains a release blocker. RX2 100-cycle final-close remains a
separate open teardown issue.
