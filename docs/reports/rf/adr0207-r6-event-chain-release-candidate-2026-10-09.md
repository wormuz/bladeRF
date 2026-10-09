# ADR-0207 event-driven RX chain candidate r6

Date: 2026-10-09

Bundle: `/home/bonho/projects/bladerf/release-staging/adr0207-event-rx-2026-10-09-r6.tar.gz`

Bundle SHA-256: `00c7c7fade8050fd66907e062fe576a7c7af0690ada04c28f8765cff67b252c2`

## Exact release pair

The bundle refreshes r5's libbladeRF with the fail-closed reload changes from
commit `e20e6e9b`. Its production `libbladeRF.so.2` SHA-256 is
`10494d03b6ed7ceab43c1331f24c5f77718da3ad2f1de956dd963d276d663474`, reports
`2.6.1-git-e20e6e9b`, has SONAME `libbladeRF.so.2`, and has no RPATH/RUNPATH.
The production build has `ENABLE_TEST_FPGA_LOAD_FAILURE_INJECTION=OFF`; neither
injection environment variable is present in the binary. The existing
CPython 3.14 wheel remains byte-identical (SHA-256
`4800cb9306ae93cb844b266fbd964ec37a1a741f0d1c7f29b7edab4ffb092b3a`).

The corrected sweep image (`43f3ce8e…77ae2f`), runtime-qualified hosted
image (`afb9b392…3113`), and FX3 firmware (`d1a979d…fc3c`) are unchanged from
r5. Every bundle member verifies against `SHA256SUMS`.

## Validation against the bundle contents

An isolated install of the bundled wheel resolves this exact library via
`LD_LIBRARY_PATH`; all 21 Python RF-event notification tests pass. The native
qualification executable selected the exact staged library and completed 10
paired RX_X2 cross-band transitions on the connected xA4 using the already
loaded hosted image. All ten had complete event histories and host-valid IQ,
zero unrecovered transitions, first-read faults, retries, or overruns, and
clean close. P50/P95/P99/max was
21.453/21.507/21.507/21.540 ms.

The recovery qualification injected an I/O failure after `BEGIN_PROG` and a
4096-byte USB bitstream prefix. The FPGA reported unconfigured; libbladeRF
refused RX operations until a fresh volatile hosted-image load. On that same
handle, the event transition confirmed PLL lock, ENSM RX, FPGA epoch, and
first host data; `sync_rx` returned 4096 samples with matching epoch META,
timestamp at/after the epoch boundary, and no overrun. RX disable and close
passed. After recovery, three cross-band transitions each passed on RX1, RX2,
and RX_X2 with clean event histories and close. No SPI flash write occurred.

The bundle contains the member hashes, Python validation output, exact-pair
smoke trace, and partial-transfer recovery report/traces.

## Release status

r6 remains a local release candidate, not final release sign-off. The FPGA
reload fail-closed recovery gate is closed for the tested partial-transfer
failure. The archived wideband LTE no-PSS RF-content case and host/NIOS RFDC
calibration policy remain open; generic detector work remains paused. Earlier
1,000/10,000-transition RX1/RX2/RX_X2 qualification and the hosted invalidation
matrix are recorded in the r5/hosted runtime reports. Timeout, sleep, or fixed
sample discard is not accepted as IQ-valid evidence.
