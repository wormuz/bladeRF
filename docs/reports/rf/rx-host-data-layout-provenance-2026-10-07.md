# RX host-data event layout provenance — 2026-10-07

## Change

`RX_FIRST_VALID_HOST_DATA` and `RX_DATA_RESUMED` now carry
`BLADERF_RF_EVENT_F_RX_X2_LAYOUT` when their validated META buffer was
delivered through `BLADERF_RX_X2`. The sync path passes its configured layout
to the board event hook; the async path already knows and now forwards the
callback layout. The Python RF-event notification adds `rx_layout: "RX_X2"`
for these paired-buffer events.

An RX_X2 META buffer contains interleaved IQ for both lanes under the shared
FPGA epoch and timestamp validation. The flag identifies that transport
layout to wrapper consumers; it does not measure detector parity or prove
independent analog sensitivity for RX1 and RX2.

## Validation

- Production libbladeRF and `libbladeRF_test_sync_epoch_traversal` build.
- Native sync traversal passes, including X2 and X1 host-data event flag
  assertions; deliberate sync timeout diagnostics are expected test cases.
- RX transition policy and RX epoch metadata tests pass.
- Python extension rebuilds against this checkout and all 14 RF-event
  notification tests pass, including RX_X2 layout mapping and overrun validity.
- No attached-board test was run for this metadata-only change.

## Remaining scope

`BLADERF_RF_REQUIRE_RX_X2_HOST_DATA` now makes the required paired layout
explicit; it rejects an active RX_X1 consumer and waits for a validated RX_X2
block. Repeated paired hardware qualification and detector parity remain open.
