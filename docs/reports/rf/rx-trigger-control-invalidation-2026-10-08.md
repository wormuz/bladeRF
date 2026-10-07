# RX trigger control invalidation (2026-10-08)

## Finding

The public `bladerf_trigger_arm()` and raw `bladerf_write_trigger()` APIs
program the FPGA trigger gate used to admit RX samples, but bypassed the
shared RX setter reservation, FPGA epoch fence, and reason-coded invalidation
event. A live certificate could therefore span a change in the capture
admission boundary without notifying libbladeRF consumers.

## Change

RX trigger arming/disarming and raw trigger-register writes now invalidate the
selected RX channel with `BLADERF_RF_INVALIDATE_TRIGGER` before the FPGA
operation and release the setter reservation afterward. If a transition or
another setter owns the reservation, the trigger write is rejected before it
reaches hardware. TX trigger operations remain RX-independent. `trigger_fire`
remains the operational edge within a configured capture and does not
reconfigure the RF/epoch contract.

The public C reason is exposed through Cython as `invalidation_reason="trigger"`.
API documentation tells consumers to establish a new event-driven RX
transition after changing RX trigger arm/configuration.

## Verification

- `libbladeRF_test_rx_shared_clock_invalidation`: pass; exercises RX2 arm and
  raw register write, confirms fence failure blocks the FPGA callback, and
  verifies TX arm/write do not invalidate the shared RX epoch.
- Cython extension rebuilt against the current libbladeRF headers.
- `tests/test_rf_event_notifications.py`: 17 passed, including the new reason
  mapping.
- `git diff --check`: clean.

No hardware trigger capture was run. This closes the API notification and
reservation gap; it does not claim triggered-capture hardware qualification.
Timeout and fixed discard remain failure/legacy mechanisms and do not
establish IQ validity.
