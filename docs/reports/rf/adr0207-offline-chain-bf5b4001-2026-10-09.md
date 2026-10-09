# ADR-0207 offline event-chain build — bladeRF bf5b4001

Date: 2026-10-09

## Result

Built a new offline package at
`/home/bonho/projects/bladerf/release-staging/adr0207-offline-20261009-bf5b4001.tar.gz`.
It contains A4 hosted/sweep images, matching NIOS ELFs, libbladeRF,
CPython 3.14 wheel, the compatible FX3 image, and the qualification evidence.
The source pair is bladeRF `bf5b4001` (no-OS submodule `b7e1fe46e`) and
python_bladerf `33cc48f`.

This is an offline release candidate, not a stable or hardware-qualified
release. No device command, reset, FPGA load, or RF measurement was performed;
the preserved xA4 state remains untouched.

## AD9361 status-read failure fix

The ADI helper `ad9361_check_cal_done()` previously stored the signed result of
`ad9361_spi_readf()` in `uint32_t`. A SPI transport error was therefore treated
as “not complete” and consumed the whole calibration watchdog (up to 24 s for
RFDC) before returning timeout. It now preserves the signed status and returns
transport errors immediately. This improves fail-fast error propagation; it
does not by itself explain the historical NIOS no-response incident.

Regression test `host/misc/run_ad9361_calibration_read_error.sh` injects `-EIO`
and verifies one SPI read, zero delay iterations, and immediate `-EIO`; its
success case verifies a completed calibration still returns immediately.

## Build and verification

- Hosted and sweep NIOS ELFs were rebuilt with the Quartus 25.1 toolchain and
  `-Werror`. Both include no-OS `b7e1fe46e`; their RAM initialization was
  regenerated before their matching FPGA fits.
- Full hosted A4 seed-5 fit passed with 0 errors. Worst setup/hold slack was
  `+0.308/+0.090 ns`.
- Full sweep A4 seed-5 fit passed with 0 errors. Worst setup/hold slack was
  `+0.231/+0.078 ns`; the separate fitted ADC transfer report paired all four
  RX bundles and found `0/240` max-skew violations.
- qgate passed for both images and `python3 hdl/quartus/qcheck` is clean.
  The original `build_bladerf.sh` console streams were not saved. The bundle
  therefore includes qgate inputs assembled from each full-fit report, a
  generated success/revision header, the matching STA/DRC reports, and the
  sweep max-skew report. The path prefixes in those qgate inputs are normalized
  to keep them usable from the bundle; the unmodified Quartus reports are
  included alongside them.
- `libbladeRF_test_nios_transaction`: PASS.
- `libbladeRF_test_sync_epoch_traversal`: exit 0 with expected cursor-repair
  and timeout diagnostics.
- AD9361 calibration SPI-error regression: PASS.
- The CPython 3.14 wheel links to `libbladeRF.so.2`, contains no RPATH/RUNPATH,
  and the staged import mapped exactly the bundled libbladeRF.
- Python RF-event tests: 21 passed. Scanner LTE-dispatch and dual-epoch tests:
  39 passed with the staged wheel and matching library. The scanner developer
  loader guard also passed separately in its intended `.envrc` environment.

## Artifact SHA-256

| Artifact | SHA-256 |
|---|---|
| Hosted A4 RBF | `216e7898f6f41d802e3d82ce9a93d18ca080cfd51e36bafd06a5e86e51a72d37` |
| Sweep A4 RBF | `282b1668f214de981c625fd7143d5ed6b18bdfb474ad7a9bbe74d69056d3283b` |
| Hosted NIOS ELF | `bef8c4df8938b8c8d7de9b96180ed254c374638f538ad0d15e753c7c8c72f86d` |
| Sweep NIOS ELF | `6b895c3f0ee5f54d58b03142c6fb22c7ea01dffa170747c3349777c198676513` |
| libbladeRF.so.2 | `e564b64e2d7039ab5608c3a3fae845eea109bc72ff2993e80e6499f324eebebe` |
| CPython 3.14 wheel | `8dbdb839bcd0b3c3a75f29dcfca318340c051ee9550145f0a1cd8d3f4bf523c1` |

Full payload and evidence hashes are in the bundle `SHA256SUMS`.

## Release gates still open

1. Exact-image xA4 hardware qualification for these new RBF/NIOS/library/wheel
   artifacts on RX1, RX2, and RX_X2.
2. NIOS ownership-mode no-response incident root cause. Preserve and collect
   first-request evidence before any FPGA reload or board reset.
3. Archived LTE no-PSS RF-content case and repeatability acceptance across
   supported RX layouts.

The offline checks do not satisfy these gates. Timeouts remain failures and
never certify IQ validity; no sleep or fixed sample discard was introduced.
