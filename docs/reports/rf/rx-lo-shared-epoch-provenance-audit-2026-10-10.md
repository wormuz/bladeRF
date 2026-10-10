# RX LO shared-epoch provenance audit (2026-10-10)

Status: source-only. No build, test, simulation, synthesis, or hardware operation was performed.

## Defect

AD9361 has a single RX LO. `ad9361_set_rx_lo_freq()` has no channel argument and updates the common RX RFPLL, while libbladeRF's public frequency setter accepts an RX channel for API consistency. The setter invalidates the shared RX1/RX2 epoch, but `BLADERF_RF_INVALIDATE_FREQUENCY` was missing from the shared-reason provenance set. When the requested channel differed from the channel that established the current epoch, the companion invalidation event could identify the setter's lane instead of the certified epoch's lane.

## Source correction

- Added frequency invalidation to shared RX provenance classification.
- Extended the existing RX1/RX2 shared-invalidation regression matrix to cover frequency and RX FIR invalidations.
- Added the corresponding focused policy assertion.

## Verification boundary

The AD9361 API confirms the RX LO operation is device-wide, and the event contract documents one shared RX LO epoch. Regression source has not been compiled or run because the complete RX1/RX2/TX source audit remains active.
