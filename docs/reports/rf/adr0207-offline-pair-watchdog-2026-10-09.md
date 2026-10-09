# ADR-0207 offline artifact pair with RX clock watchdog — 2026-10-09

## Result

Built a reproducible offline artifact set from bladeRF source commit
`ca60fafd` and Python binding commit `33cc48f`:

`/home/bonho/projects/bladerf/release-staging/offline-20261009-ca60fafd/`

This is a static review bundle, not a release candidate. It contains the
production host library, CPython 3.14 wheel, hosted and sweep A4 RBFs, matching
hosted and sweep NIOS ELF diagnostics, and the locally built FX3 image.
`SHA256SUMS` records all seven payload hashes.

## Verification

- Hosted and sweep Quartus A4 builds completed with zero errors. Neither has
  negative setup/hold slack. `qgate` passed for both generated fits;
  `qcheck` passed. The sweep max-skew report checked 240 paths with zero
  violations (worst slack +3.303 ns).
- `if_clock_watchdog_tb` passed absent/running/stopped/restarted clock cases.
- Hosted and sweep NIOS firmware builds passed.
- CMake production `libbladeRF_shared` and native sync epoch traversal passed.
- The staged wheel passed 29 scanner RX-transition/dual-epoch tests and 21
  Python RF-event notification tests.
- Importing the staged wheel with `LD_PRELOAD` unset mapped the exact staged
  `lib/libbladeRF.so.2`.
- Rebuilt CLI identifies the host library source stamp as
  `1.10.0-git-ca60fafd`.

## Payload SHA-256

| Artifact | SHA-256 |
|---|---|
| `lib/libbladeRF.so.2` | `623f51ed644945b28d5eb74bc82cb3ea9b6426fc4096b294f410fc1f51b171fd` |
| `wheel/python_bladerf-1.5.0-cp314-cp314-linux_x86_64.whl` | `042efa7663f0cc29adef40945cae2bf116aae97f9f1fb590f90644eaec22da72` |
| `fpga/hosted.rbf` | `e09d3e55162c9c039a9b255bef3ae766b81cbb2d82c8d02a4db9c4bd25e409da` |
| `fpga/sweep.rbf` | `e477b0da5802ec8d648e72ee3d58369ac5d91922103dadd7d19c8a5ac31ddb6f` |
| `nios/hosted-bladeRF_nios.elf` | `ff33470c4771bdae3acdf20fb3d6ff2f002ff66d9bc5482bb23ddfd63b86c622` |
| `nios/sweep-bladeRF_nios.elf` | `741584e8c73f36cecd8268ffb4ec10844f9a9075c1b2881baf46828319274e65` |
| `fx3/bladeRF_fw_v2.6.1.img` | `d2ca89f53de7da0b190eccbae5a6aa9eb5fcb49421b30022403c213f71d77064` |

## Limits and remaining P0

No FPGA image was loaded and the preserved xA4 NIOS no-response state was not
disturbed. Consequently this package has no runtime hardware qualification.
The independent RX interface-clock guard is preventive: it catches a missing
clock before NIOS enters the raw unbounded AD9361 AXI adapter. It cannot
interrupt an AXI transaction if the clock stops after entry and is not a
proven explanation of the preserved mode-switch failure. The first-request
NIOS freeze diagnosis, physical RX/TX transport faults, and the archived LTE
no-PSS RF-content case remain open. Do not call this a stable release until
those release gates are closed on the exact images and pair.

Full hosted compile log was not persisted; the hosted `qgate` input was
reconstructed from the generated fitter/STA/DRC reports plus revision
metadata. Sweep log: `/tmp/sweep-a4-if-clock-watchdog-20261009.log`.
