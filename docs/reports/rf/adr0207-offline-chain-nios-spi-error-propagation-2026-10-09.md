# ADR-0207 offline chain build after NIOS AD9361 SPI error propagation — 2026-10-09

## Result

The headless NIOS AD9361 SPI adapter now propagates both failed SPI reads and
writes into no-OS instead of returning success with zero or uninitialized
data. The checked read validates the Avalon SPI result and initializes its
receive buffer; legacy NIOS callers retain the old value-returning helper.
Source commit: bladeRF `777823e8`. The firmware's parent no-OS AXI error
propagation commit `00daad717` and this bladeRF branch are pushed to the
`wormuz` forks.

Full hosted and sweep A4 images were rebuilt from the resulting clean source
tree. The exact offline artifact set is
`/home/bonho/projects/bladerf/release-staging/offline-20261009-d80b06ef/`.
The image snapshot identifies bladeRF commit `d80b06ef`, which adds only
qualification traces after the firmware source commit.

## Verification

- Both complete Quartus builds completed with zero errors; full build logs
  are `/tmp/hosted-spi-error-propagation-20261009.log` and
  `/tmp/sweep-spi-error-propagation-20261009.log`. `qgate` passed on both
  exact logs; `hdl/quartus/qcheck` is clean. Neither STA report has negative
  slack. Sweep post-fit analysis paired four 61-bit RX bundles, all 9 ADI
  transfer bundles, 2 clock-monitor bundles and 6 status bundles. Max-skew
  found 240 paths with zero violations; worst slack was +3.303 ns.
- NIOS hosted and sweep ELFs are separately captured in the artifact set.
- Host CMake was reconfigured to refresh its generated version stamp; the
  production library is `2.6.1-git-d80b06ef`. The CLI reports
  `1.10.0-git-d80b06ef`. The native sync epoch traversal passed.
- The CPython 3.14 no-RPATH wheel was rebuilt from python-bladerf `33cc48f`
  against the staged library. 29 scanner RX-transition/dual-epoch tests and
  21 wrapper RF-event tests passed. With `LD_PRELOAD` unset, `/proc/self/maps`
  confirmed the exact staged `lib/libbladeRF.so.2` was loaded.

## Artifact SHA-256

The full manifest is `SHA256SUMS` in the stage directory. Key payloads:

| Artifact | SHA-256 |
|---|---|
| `lib/libbladeRF.so.2` | `a5c4ae0e3d3b7454ce34b65f7ed34db0283edadb471f3ab2387fa0a56982e124` |
| `wheel/python_bladerf-1.5.0-cp314-cp314-linux_x86_64.whl` | `4b26547c50c55d41364643d33e2020e2919b00cb602400a8afb71bdd0e140852` |
| `fpga/hosted.rbf` | `751042dfd8889225d5f46f9e2f00b0109857e1ed3720f144c90383ad49e06282` |
| `fpga/sweep.rbf` | `02fe7a3ee7d3d6676dd13f91e2381239e66ee858a1599cb70d28ede692b55233` |
| `nios/hosted-bladeRF_nios.elf` | `0c6ebb5de115378bdf22fd77f79fe5ad85dd98249a8fe7d40b3347c11f70a63c` |
| `nios/sweep-bladeRF_nios.elf` | `fb9560c55c84a57c62d7e768a1b5ebe711660fb89a87ef09d0704605afc304ab` |
| `fx3/bladeRF_fw_v2.6.1.img` | `d2ca89f53de7da0b190eccbae5a6aa9eb5fcb49421b30022403c213f71d77064` |

## Limits

Offline build and API qualification do not make this a stable release. No
image was loaded, and the preserved xA4 NIOS no-response state was not
disturbed. The SPI propagation change closes a false-success/error-reporting
gap; it does not establish the cause of that no-response. ADI timed-out AXI
writes still have no explicit timeout response. Exact-image runtime RX1,
RX2, and RX_X2 qualification and the archived LTE no-PSS RF-content cause
remain release gates.
