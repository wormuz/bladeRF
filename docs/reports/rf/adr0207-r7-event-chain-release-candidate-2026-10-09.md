# ADR-0207 event-driven RX chain candidate r7

Date: 2026-10-09

Bundle: `/home/bonho/projects/bladerf/release-staging/adr0207-event-rx-2026-10-09-r7.tar.gz`

Bundle SHA-256: `86c78d95359ad829caec3af3f4cf5b692459a7ce9e4986ac5a49a6d2499b5eea`

## Exact artifacts

r7 carries the same production artifacts as r6, including libbladeRF
`2.6.1-git-e20e6e9b` (SHA-256
`10494d03b6ed7ceab43c1331f24c5f77718da3ad2f1de956dd963d276d663474`), with
test injection OFF. The wrapper wheel, corrected sweep RBF, hosted RBF, and
FX3 firmware hashes are unchanged. The archive's 20 file members verify
against its `SHA256SUMS`.

## Validation

The exact bundled wheel resolves this staged library; all 21 Python RF-event
tests pass. With the exact staged library and already-loaded hosted image, the
hardware qualification harness completed 100 cross-band transitions per
layout:

| Layout | Success | P50 / P95 / P99 / max (ms) | First-read faults | Retries/overruns |
|---|---:|---:|---:|---:|
| RX1 | 100/100 | 20.157 / 20.455 / 20.619 / 20.626 | 0 | 0 |
| RX2 | 100/100 | 20.044 / 20.380 / 20.770 / 20.972 | 0 | 0 |
| RX_X2 | 100/100 | 21.458 / 23.480 / 24.667 / 28.542 | 0 | 0 |

All three runs had complete event histories, no unrecovered transitions, and
clean RX1/RX2/device teardown. The raw logs are in the r7 bundle. The same
candidate also passed the 4096-byte partial-transfer failure case, fail-closed
behavior, same-handle recovery, and a host META RX read after recovery.

## Status

r7 provides repeated exact-candidate RX1, RX2, and paired RX_X2 validity
evidence, but it remains a local release candidate. The archived wideband LTE
no-PSS RF-content case and Host/NIOS RFDC policy remain open. Generic detector
development remains paused until the chain release gates are resolved. No
timeout, sleep, or fixed sample discard was used as IQ-valid evidence.
