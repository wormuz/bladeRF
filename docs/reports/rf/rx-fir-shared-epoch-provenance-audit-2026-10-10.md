# RX FIR shared-epoch provenance audit (2026-10-10)

Status: source-only. No build, test, simulation, synthesis, or hardware operation was performed.

## Defect

The bladeRF2 RX FIR setter uses the AD9361 decimate-by-1/2/4 FIR configurations whose `rx` mask is `3` (RX1 and RX2). The invalidation path correctly fences the shared RX epoch, but its companion `RX_INVALIDATION_CHANNEL` event classified `RX_FIR` as lane-specific. Because the setter internally selects RX1, an epoch established on RX2 was incorrectly attributed to RX1 in the event provenance.

## Source correction

- Added `BLADERF_RF_INVALIDATE_RX_FIR` to shared-reason classification.
- Centralized that classification in `bladerf2_rx_invalidation_reason_is_shared()` and used it when deriving event provenance.
- Added source regression assertions for RX FIR, raw RFIC register, and lane-specific gain invalidation classifications.

## Verification boundary

Source review confirms the checked-in RX FIR configurations use `rx=3`. The regression has not been compiled or run because the full RX1/RX2/TX source defect sweep remains active. No build/test/device operation was performed.
