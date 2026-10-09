# ADR-0207 event-driven RX release candidate bundle

Date: 2026-10-09

## Bundle

Local artifact: `release-staging/adr0207-event-rx-2026-10-09.tar.gz`  
SHA-256: `48e2aba1ce4542f142f05f16f6244d751153e30965332561e011b021533d99ac`

The tarball contains a manifest, checksums, the matched libbladeRF shared
library, the exact-source FX3 firmware build, the hardware-qualified A4 RBF,
and the CPython 3.14 no-RPATH wheel. Member checksums are listed in the
bundle's `SHA256SUMS`.

## Selection and live identity

- libbladeRF `2.6.1-git-3c8b7ff4`, SHA-256
  `001d519963a2f6849e7a8b30b64c3a11c5ac7cf9c00b31d059518558a4204c84`.
- FX3 firmware source commit `fc02ffd8`, rebuilt image reports
  `2.6.1-git-fc02ffd8`, SHA-256
  `d1a979d164f81222a8463526e589e82f40cfd9d47df466cb1ef5488d6608fc3c`.
  The xA4 currently reports the same firmware version.
- FPGA build 000074, source `0391ce11`, seed 7, RBF SHA-256
  `ab7aefef2fa72fde0cd7cd2ba67853a79aaf86b7f0f6dda4879657dea050d5d0`.
  This image passed its recorded qgate and was last loaded volatilely on xA4
  serial `f695006ba84a40daa7b777c6c6eba78`. No flash write was performed.
- Python wheel SHA-256
  `55695aaffa1205ac6dde508a48a94b205e57ec8772a2bbea069b7704d37e90ac`.

The xA4 reports firmware `2.6.1-git-fc02ffd8` and FPGA `0.16.1`. The installed
library reports `2.6.1-git-3c8b7ff4`. The staged wheel resolves that same
system library with `LD_PRELOAD` and `LD_LIBRARY_PATH` unset. The previous
system library is backed up at
`/home/bonho/.local/state/bladerf/system-library-backups/20261009/libbladeRF.so.2.pre-3c6466af`.

## Qualification linked to this candidate

- Exact bundled library and already-running FPGA: 10,000 paired RX_X2
  cross-band transitions at 23.04 Msps and the 32k/64/32 production geometry,
  with zero read faults, retries, or stream overruns. All post-certificate
  withheld packets mapped to the correct transaction across epoch wrap.
  Report: `rx-epoch-release-candidate-3c8b7ff4-rxx2-10k-2026-10-09.md`.
- Exact bundled library/no-RPATH wheel: 200/200 production LTE transitions
  valid, 100/100 known-cell PCI85/100RB/four-port returns, no short reads or
  overruns, and no target no-PSS capture. Report:
  `/home/bonho/projects/sdr-scanner/docs/reports/rf/lte-release-rxx2-3c8b7ff4-100-20261009.md`.
- Exact bundled library SHA: 10,000/10,000 paired transitions at 23.04 Msps,
  full event history, clean RX1/RX2/device teardown, and zero unrecovered or
  first-read faults, WOULD_BLOCK retries, or overruns. P50 21.807 ms, P95
  22.433 ms, P99 23.103 ms, max 31.465 ms. Report:
  `rx-epoch-release-candidate-rxx2-10k-20261009.md`.
- Withheld-event source provenance: native traversal regression plus live
  100-transition smoke and 10,000-transition repeat. Source epoch/timestamp
  showed that the two extra callbacks were stale prior-epoch USB packets;
  none reached application IQ. `transaction_id` remains zero for stale data.
  Report: `rx-epoch-withheld-notification-detail-2026-10-09.md`.
- Matched system library/wheel: two consecutive 100-return production LTE
  RX_X2 cycles, all 200 target returns decoded PCI85/100RB/four ports. Report:
  `/home/bonho/projects/sdr-scanner/docs/reports/rf/lte-matched-release-rxx2-100-20261009.md`.
- Live independent RX1/RX2 and paired RX_X2 transition/capture-close smoke.
  Report:
  `/home/bonho/projects/python_bladerf/docs/release-wheel-linkage-2026-10-09.md`.

The newer job 000075 RBF was excluded: its separate fit passed timing and
max-skew but failed qgate on D101 source classification and critical-warning
disposition. It is not the qualified image in this bundle.

## Release limits

This is a reproducible local candidate bundle, not a final release. The
archived wideband no-PSS/RF-content case remains unexplained. The bundle was
not flashed, published, or installed as firmware.
