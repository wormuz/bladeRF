# qgate exact STA report resolution — 2026-10-07

## Finding

`hdl/quartus/qgate` searched for a missing timing summary under the log's parent directory. When a Quartus log was stored in `/tmp`, that scope became `/`, so the gate traversed unrelated directories and could read a stale `.sta.summary` from a different build. On the RX FIFO reset CDC compile this produced a false negative-slack result, despite the build's own timing report showing worst hold slack `+0.023 ns` and setup slack `+3.125 ns`.

A missing timing summary was previously reported as “not checked” without failing the gate. A successful Quartus compile could therefore pass qgate without timing evidence.

## Change

qgate now resolves the STA summary using the exact Quartus `Project Name` and `Revision Name`, with only exact-revision adjacent fallbacks. It does not search the log's parent tree. If no matching summary is available, qgate fails closed.

Added `hdl/quartus/test_qgate.sh`, a durable regression that stores the log outside the project, puts a negative-slack decoy near the log, verifies that the exact project's positive-slack report is selected, then verifies that a missing report fails.

## Verification

- `hdl/quartus/test_qgate.sh` — PASS.
- `hdl/quartus/qcheck` — clean.
- qgate re-run on `/tmp/adr0207-rx-fifo-reset-sync-quartus.log` now reports `timing (no negative slack)` using the build's own STA report. The overall gate remains failed by its existing 64 Quartus Critical Warning records (C105/D101/D103); these were not waived or hidden.
- `git diff --check` — PASS.

This corrects build evidence selection and prevents broad filesystem traversal. It does not resolve the outstanding CDC warnings or qualify the image for loading.
