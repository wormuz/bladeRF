# ADR-0207 current libbladeRF/Cython software pair, offline qualification

Date: 2026-10-09

## Exact pair

Staged at `/home/bonho/projects/bladerf/release-staging/adr0207-offline-software-pair-aaf92b3e/`.

- libbladeRF source: commit `aaf92b3e`, version `2.6.1-git-aaf92b3e`, SHA-256 `a0cea43bbff86d71d17c8dc205cc2f3d16a17408b6f3bcb0bc71cb638d776d2b`
- Cython wrapper source: commit `33cc48f`, CPython 3.14 x86_64 wheel SHA-256 `b4480e427000ffffee202573dcf6a0ea3c5d7dd32cc14cc4923f0fb4533aa9bc`
- Both hashes are recorded in the staged `SHA256SUMS`.

CMake had retained an old generated version header (`2.6.1-git-e20e6e9b`) even though it rebuilt current source. Reconfiguring `host/build` refreshed the version stamp to `aaf92b3e`; the production shared-library target was rebuilt from that configuration. The build has no RX SPI test-injection symbol, the library has SONAME `libbladeRF.so.2`, and it has no RPATH/RUNPATH.

## Verification

- Full configured host build had passed in the preceding source step; after refreshing the version stamp, `libbladerf_shared` and `libbladeRF_test_sync_epoch_traversal` rebuilt successfully.
- `host/build/output/libbladeRF_test_sync_epoch_traversal` exited 0. Its timeout logs are expected fault-path cases in the test.
- Release wheel was rebuilt with `PYTHON_BLADERF_RELEASE_BUILD=1` against current bladeRF headers and this exact library. The primary wrapper, sweep, and scan extensions have `NEEDED libbladeRF.so.2` and no RPATH/RUNPATH.
- Extracted wheel imported from `/tmp` with `LD_PRELOAD` unset; `/proc/self/maps` showed exactly the staged `libbladeRF.so.2` when `LD_LIBRARY_PATH` pointed to the isolated pair.
- RX transition API and dual-epoch wrapper test set in `sdr-scanner`: 29 passed.
- Python RF-event callback/notification test set in `python_bladerf`: 21 passed.
- Total focused wrapper/event tests against the exact pair: 50 passed.

## Limits

This is an exact, hash-paired software artifact with offline native and Python API evidence. It contains no FPGA/NIOS/FX3 images and has not been hardware-qualified. It is explicitly not a release candidate; it does not close the NIOS no-response, archived LTE no-PSS, exact hardware soak, or final release-bundle gates. No device access or recovery was performed.
