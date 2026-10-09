# RX withheld-event transaction provenance across epoch wrap

Date: 2026-10-09

## Change and native regression

libbladeRF now maps a withheld META packet's source epoch/timestamp to the
latest retained `RX_EPOCH_VALID` event with the same 8-bit epoch and a
certificate timestamp no later than the packet timestamp. The resulting
`RX_DATA_WITHHELD` event carries the source transition ID, requested/readback
LO, RFIC status, and source channel flags. If the matching certificate has
already left the event history, the event remains unassociated (`transaction_id=0`).

The sync/epoch traversal test constructs certificates for epoch sequence
255→0→255 and checks packet timestamps on both sides of the wrap. It verifies
that timestamp selects transaction 42 or 44 as appropriate, with the matching
LO/RFIC/channel provenance. The native traversal suite passes.

## Live xA4 qualification

- Installed and tested libbladeRF: `2.6.1-git-3c8b7ff4`.
- SHA-256: `001d519963a2f6849e7a8b30b64c3a11c5ac7cf9c00b31d059518558a4204c84`.
- RX_X2 / BOTH, alternating 947.5 MHz ↔ 1.835 GHz, 23.04 Msps,
  32,768 samples/channel, 64 buffers / 32 transfers.
- 10,000/10,000 full paired captures; complete transition history 1–10000;
  zero unrecovered, first-read, recovered, WOULD_BLOCK, retry-overrun, or
  stream-overrun counts; clean RX1/RX2 disable and device close.
- Latency P50 21.806 ms, P95 22.454 ms, P99 22.644 ms, maximum 37.370 ms.
- 10,001 withheld events: the initial pre-certificate event correctly keeps
  transaction ID 0; all post-certificate source events have a transaction ID.
  One transaction produced two withheld buffers from the same old epoch at
  distinct timestamps 114,464 samples apart, and both mapped to that same
  transaction. Epoch ID wrapped several times without misassociation.

No withheld IQ was delivered to the caller. This closes the stale-callback
transaction-correlation gap for the tested event-history retention window.
Raw local trace: `docs/reports/rf/rx-epoch-transaction-map-release-rxx2-10k-20261009.log`.
