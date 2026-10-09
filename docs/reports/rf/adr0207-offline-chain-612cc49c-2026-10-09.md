# ADR-0207 offline chain build at `612cc49c` — 2026-10-09

## Result

Rebuilt the hosted and sweep A4 FPGA images after the no-OS AXI error
propagation change, then rebuilt the host library from a freshly configured
CMake tree so its generated version stamp matches bladeRF source commit
`612cc49c`. The offline pair is staged at
`/home/bonho/projects/bladerf/release-staging/offline-20261009-612cc49c/`.
The stage includes libbladeRF, the CPython 3.14 wheel, hosted/sweep RBFs,
hosted/sweep NIOS ELFs, FX3 image, and `SHA256SUMS`.

## Verification

- Hosted and sweep A4 full Quartus builds completed successfully. `qgate`
  passed for both reconstructed inputs; `qcheck` is clean. Sweep post-fit CDC
  inventory paired all four RX bundles, all nine ADI transfer bundles, both
  clock-monitor bundles and all six transfer-status bundles; max-skew had no
  violations. No setup/hold slack was negative.
- The native `libbladeRF_test_sync_epoch_traversal` passed after the final
  CMake reconfigure and library rebuild.
- The CPython 3.14 no-RPATH wheel was rebuilt from python-bladerf commit
  `33cc48f` against bladeRF headers and this library. The staged wheel passed
  29 scanner RX-transition/dual-epoch tests and 21 RF-event notification
  tests. With `LD_PRELOAD` unset, the imported Cython extension mapped the
  exact staged `lib/libbladeRF.so.2`.
- `bladeRF-cli --version` reports `1.10.0-git-612cc49c`.

## Artifact SHA-256

See the stage's `SHA256SUMS`. Key payloads:

| Artifact | SHA-256 |
|---|---|
| `lib/libbladeRF.so.2` | `fc6f4a8802d26277c761c00e8251d1a25cf817b820321c7f6075f10fcbe91d75` |
| `wheel/python_bladerf-1.5.0-cp314-cp314-linux_x86_64.whl` | `0d033f6cdec6eaf108516ddab44161408f33de5435313a6ffc1684c52464be34` |
| `fpga/hosted.rbf` | `5338a68ec51a7ac3deefa20339c4c8d9ebe3f54396b70680cbe45dca666ece2a` |
| `fpga/sweep.rbf` | `492c5ced782f5d9ca683faedf986fc6086b994e5d5e13ecaa3597cf3caafde2c` |
| `nios/hosted-bladeRF_nios.elf` | `2bf618b955dd7eb11446743ec0091994e70d22dd02d01b536d596b93826e4044` |
| `nios/sweep-bladeRF_nios.elf` | `00584ede14538702abc754c74a7a82764b692c55c4dd6a381ecda5008eea1efd` |
| `fx3/bladeRF_fw_v2.6.1.img` | `d2ca89f53de7da0b190eccbae5a6aa9eb5fcb49421b30022403c213f71d77064` |

## Limits

This is offline build and API qualification, not a stable release. No image
was loaded and the preserved xA4 NIOS no-response state was not disturbed.
The root cause of that no-response remains open; `up_axi` timeout reads are
now rejected in firmware, while timed-out ADI writes still appear as AXI
OKAY. Runtime RX1/RX2/RX_X2 qualification on this exact artifact set and the
archived LTE no-PSS RF-content cause remain release gates. Do not label this
pair hardware-qualified or sign the stable release until those gates pass.

Evidence files: `/tmp/hosted-a4-20261009-qgate.log`,
`/tmp/sweep-a4-20261009-qgate.log`, and
`/tmp/sweep-postfit-cdc-20261009.log`.
