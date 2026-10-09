# ADR-0207 offline event-chain candidate — bladeRF cc38e59e

Date: 2026-10-09

## Candidate

This offline package matches bladeRF `cc38e59e`, no-OS `b7e1fe46e`, and
python_bladerf `33cc48f`. It contains hosted and sweep A4 seed-5 RBF images,
their NIOS ELFs and RAM init, `libbladeRF 2.6.1-git-cc38e59e`, a CPython 3.14
no-RPATH wheel, and the previously qualified FX3 image.

This remains an offline candidate, not a stable or hardware-qualified release.
No image was loaded during this work. The xA4 remains preserved after its NIOS
no-response incident. Incident diagnosis, exact-image runtime qualification
on RX1/RX2/RX_X2, and archived LTE no-PSS RF-content acceptance remain open.
The matched bundle is staged at
`/home/bonho/projects/bladerf/release-staging/adr0207-offline-20261009-cc38e59e.tar.gz`.

## NIOS I2C failure-reporting change

Commit `cc38e59e` closes the next static audit gap. Bounded Si5338/INA219 I2C
transfer failures were hidden by zero-return/void accessors, causing NIOS 8x8,
8x16, and legacy responses to claim success. Accessors now return explicit
status, read outputs clear on failure, and packet/legacy paths propagate the
failure. Existing libbladeRF 8x8/8x16 success-bit handling maps it to
`BLADERF_ERR_FPGA_OP`; legacy access uses the error marker.

The candidate also includes earlier AD9361 and peripheral SPI failure
propagation fixes from `82ae6671` and `71fa6df2`.

## Build and verification

- Hosted and sweep A4 seed-5 full Quartus 25.1 builds completed with zero
  errors. qgate passed against the exact archived reports. Hosted worst setup/
  hold slack is `+0.308/+0.090 ns`; sweep is `+0.231/+0.078 ns`. Sweep
  max-skew is 0 violated paths out of 240, worst slack `+3.075 ns`.
- Both NIOS applications compiled with `-Werror`. RAM init hashes match:
  `2a721541ef5b41da4b4f8727e85a6623ae4291a244cd6d4d47876ca78ca234b9`.
- The I2C packet regression covers INA219 read/write failure and success and
  Si5338 8x8 failure. Peripheral SPI, legacy error marker, RFPLL packet/host,
  retune queue, AD9361 calibration error, NIOS transaction, and sync epoch
  traversal tests pass. `qcheck` is clean.
- The host library was reconfigured after commit `cc38e59e` and reports
  `2.6.1-git-cc38e59e`; SHA-256 is
  `4fe3bdcfa6fce0318ae2fb63817ae607d9d8714a24903553e181b368c535b587`.
- The CPython 3.14 release wheel has SHA-256
  `096a4fe23ee39b5adb94f57deff66190fdded6b816f712c27079e2faf8dcb0be`.
  `readelf` shows `NEEDED libbladeRF.so.2` and no RPATH/RUNPATH. Isolated
  import mapped the extension to the exact paired library and found RX
  transition begin/wait and RF event APIs. Wrapper tests: 31 passed. Scanner
  RX transition, epoch and LTE dispatch/capture tests: 64 passed.
- Quartus report files and qgate evidence are archived in this bundle. The
  original full build console streams were not retained; the package keeps the
  exact `.map`, `.fit`, `.sta`, `.drc`, and `.flow` reports instead.

## Artifact hashes

| Artifact | SHA-256 |
|---|---|
| Hosted A4 RBF | `cfe66f555443617b1117d40d47efd86105a2e2e1fc39417d00cd54514f9f587a` |
| Sweep A4 RBF | `3e5934932bd502f41af5aac231a1f9c9d850fa26afc5310448ed6676df886308` |
| Hosted NIOS ELF | `0eec851920e165716ef655c83cd3511a9fed71130580e0b28bfdb94ed9e6552e` |
| Sweep NIOS ELF | `3b52f89899dadc8583347f314828e47e17a7f8d4703dfcc5565bef33d8fac98f` |
| RAM init (both) | `2a721541ef5b41da4b4f8727e85a6623ae4291a244cd6d4d47876ca78ca234b9` |
| libbladeRF.so.2 | `4fe3bdcfa6fce0318ae2fb63817ae607d9d8714a24903553e181b368c535b587` |
| CPython 3.14 wheel | `096a4fe23ee39b5adb94f57deff66190fdded6b816f712c27079e2faf8dcb0be` |
