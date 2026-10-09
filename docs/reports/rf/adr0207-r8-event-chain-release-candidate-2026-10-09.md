# ADR-0207 event-driven RX chain candidate r8

Date: 2026-10-09

Bundle: `/home/bonho/projects/bladerf/release-staging/adr0207-event-rx-2026-10-09-r8.tar.gz`

Bundle SHA-256: `7447c59234d11173c5058caeed5023510991b6cf9d1c2ee3c9f8143c9f001117`

## Exact release pair

r8 carries the r7 production library, wrapper wheel, corrected sweep image,
hosted image, and FX3 firmware. The production library is
`2.6.1-git-e20e6e9b`, SHA-256
`10494d03b6ed7ceab43c1331f24c5f77718da3ad2f1de956dd963d276d663474`; test
injection is OFF, SONAME is `libbladeRF.so.2`, and RPATH/RUNPATH are absent.
The archive's 22 file members verify against `SHA256SUMS`.

The isolated bundled wheel resolves this exact library and all 21 Python
RF-event tests pass. The exact staged library and hosted image completed
1,000 cross-band transitions each in RX1, RX2, and RX_X2:

| Layout | Success | P50 / P95 / P99 / max (ms) | Faults, retries, overruns |
|---|---:|---:|---:|
| RX1 | 1,000/1,000 | 19.930 / 20.351 / 20.966 / 26.824 | 0 |
| RX2 | 1,000/1,000 | 20.016 / 20.499 / 21.357 / 27.245 | 0 |
| RX_X2 | 1,000/1,000 | 21.451 / 21.570 / 22.497 / 29.605 | 0 |

Each run had complete event history, valid epoch-tagged captures, zero
unrecovered transitions, first-read faults or recovery attempts, and clean
close. The raw traces are included in the bundle.

The same artifacts passed a partial-transfer FPGA failure test after a
4096-byte bitstream prefix. The library failed closed with an unconfigured
FPGA, refused RX operations, accepted a fresh volatile image load, then
completed same-handle PLL/ENSM/epoch/first-host-data and returned 4096
matching epoch META samples without overrun. No SPI flash write occurred.

## Release status

r8 is a local release candidate, not final sign-off. Exact-artifact RX1, RX2,
and paired RX_X2 event/data validity is qualified at 1,000 transitions per
layout. The archived wideband LTE no-PSS RF-content case and Host/NIOS RFDC
policy remain open. Generic detector work remains paused. Timeout, sleep, and
fixed sample discard are not used to certify IQ.
