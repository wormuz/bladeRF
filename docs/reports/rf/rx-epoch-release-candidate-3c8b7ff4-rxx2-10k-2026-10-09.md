# Exact transaction-provenance release RX_X2 10,000-transition gate

Date: 2026-10-09

## Artifact identity

- Installed and bundled libbladeRF: `2.6.1-git-3c8b7ff4`.
- SHA-256: `001d519963a2f6849e7a8b30b64c3a11c5ac7cf9c00b31d059518558a4204c84`.
- Native harness RPATH resolved the byte-identical
  `host/build/output/libbladeRF.so`.
- xA4 serial `f695006ba84a40daa7b777c6c6eba78`, already-running qualified
  build-000074 FPGA image; no FPGA reload or flash write.

## Conditions and result

- RX_X2 / BOTH, cross-band 947.5 MHz ↔ 1.835 GHz.
- 23.04 Msps, 32,768 samples/channel, 64 buffers / 32 active transfers.
- 10,000 transitions and 10,000 terminal capture closes.
- 10,000/10,000 full captures; event history 1–10000 complete; RX1/RX2
  disable and device close succeeded.
- Zero unrecovered or first-read faults, recovered reads, WOULD_BLOCK retries,
  retry overruns, or stream overrun events.
- Latency P50 21.806 ms, P95 22.454 ms, P99 22.644 ms, max 37.370 ms.
- 10,001 withheld notifications. The initial pre-certificate event has ID 0;
  all post-certificate packets resolve to a retained transaction. One
  transaction produced two distinct old-epoch buffers 114,464 samples apart;
  both mapped to that transaction. The 8-bit FPGA epoch wrapped repeatedly
  without misassociation.

The source packet epoch, FPGA timestamp, transaction ID, requested/readback
LO, RFIC status, and source channel flags are preserved. No withheld IQ was
delivered. The wrap-aware native regression is in
`host/libraries/libbladeRF_test/test_sync_epoch_traversal/main.c`.

Raw local trace:
`docs/reports/rf/rx-epoch-transaction-map-release-rxx2-10k-20261009.log`.
