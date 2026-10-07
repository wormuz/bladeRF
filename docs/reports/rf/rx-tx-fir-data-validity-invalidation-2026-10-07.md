# TX FIR changes revoke RX data validity

Date: 2026-10-07

## Finding

`bladerf_set_rfic_tx_fir()` programs the AD9361 TX FIR. The vendor API describes FIR configuration as potentially affecting the data path, but the setter previously changed shared RFIC clock/datapath state without revoking an existing RX epoch certificate. Applications could therefore continue to receive IQ presented as valid after a shared RFIC datapath mutation.

## Change

The setter now reserves the RX transition path and publishes `RX_DATA_INVALIDATED` with the new `BLADERF_RF_INVALIDATE_TX_FIR` reason before programming TX FIR registers. On success, the shared RX certificate remains revoked until the caller completes a new event-driven RX transition. Errors release the setter reservation through the common reconfiguration completion path.

The reason uses bit 25. A first wrapper test caught that bit 24 is already assigned to `BLADERF_RF_INVALIDATE_FPGA_RX_LOSS_STATUS_UNAVAILABLE`; keeping the values distinct preserves one-to-one reason-name reporting.

The Python binding exports `RF_INVALIDATE_TX_FIR` and maps it to the event reason string `tx_fir`.

## Verification

- Full host CMake build passed.
- `run_rx_transition_validity_live.sh` passed on the xA4 in RX1, RX2, and paired RX1+RX2 layouts. Each run confirmed the TX FIR invalidation reason, `bladerf_sync_rx()` withheld IQ with zero samples, and a subsequent event-driven transition restored IQ.
- The live validity test now captures an event-history cursor immediately before TX FIR programming, so earlier ring contents or bounded-history truncation cannot mask the new event.
- The paired run logged one transient sync worker buffer timeout during the existing timeout-boundary check; the check and full paired scenario passed, and no invalid IQ was admitted.
- Python extension build passed; `tests/test_rf_event_notifications.py` passed (12 tests), including uniqueness and naming for every public invalidation reason.
- Native RX shared-clock invalidation and sync epoch traversal tests passed. The two deliberate 1 ms sync waits in the native traversal test emitted expected timeout diagnostics.

## Scope

The certificate and FPGA RX epoch are shared by RX1 and RX2. The live matrix verifies both single-channel selections and simultaneous RX1+RX2 streaming. The transition timeout remains an error outcome and does not certify IQ.
