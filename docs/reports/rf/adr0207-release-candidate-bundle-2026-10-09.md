# ADR-0207 event-driven RX release candidate bundle

Date: 2026-10-09

## Bundle

Local artifact: `release-staging/adr0207-event-rx-2026-10-09.tar.gz`  
SHA-256: `79666c5cbd07e441978550a8e5eb519353061a6ab40e88f78c067e8ed711424d`

The tarball contains a manifest, checksums, the matched libbladeRF shared
library, the exact-source FX3 firmware build, the hardware-qualified A4 RBF,
and the CPython 3.14 no-RPATH wheel. Member checksums are listed in the
bundle's `SHA256SUMS`.

## Selection and live identity

- libbladeRF `2.6.1-git-58401a8a`, SHA-256
  `ae3cd253ab2532f3ba875dc7481a69a1bb161387a67ac4799fc5ef92fc34b7f8`.
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
library reports `2.6.1-git-58401a8a`. The staged wheel resolves that same
system library with `LD_PRELOAD` and `LD_LIBRARY_PATH` unset.

## Qualification linked to this candidate

- Current library and already-running FPGA: 10,000 paired RX_X2 cross-band
  transitions at 23.04 Msps and the 32k/64/32 production geometry, with zero
  read faults or stream overruns. Report:
  `rx-epoch-matched-release-rxx2-10k-23msps-2026-10-09.md`.
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

This is a reproducible local candidate bundle, not a final release. Historical
no-PSS IQ anomalies remain unexplained, and the production-rate event collector
counted 10,002 `RX_DATA_WITHHELD` notifications across 10,000 transitions; two
notifications lack event-level detail in that harness output. The bundle was
not flashed, published, or installed as firmware.
