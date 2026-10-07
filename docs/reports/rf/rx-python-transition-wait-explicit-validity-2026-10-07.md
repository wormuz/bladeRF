# Python transition wait reports IQ validity explicitly

Date: 2026-10-07

## Finding

`bladerf_rx_transition_wait()` can succeed at different contract levels. In
particular, a control-plane-only request can complete with
`RX_DATA_INVALID` / `CONTROL_PLANE_CONFIRMED`; the native event history and
Python event poller mark that state as `iq_valid: false`. The direct Python
`pybladerf_rx_transition_wait()` result omitted `iq_valid`, `event_name`, and
the validity details, so callers receiving only the method result had to infer
whether success certified data.

## Change

The direct wait result now uses the same event-name, invalidation-reason, and
validity mapping as event history and callbacks. A successful control-plane
wait explicitly returns `iq_valid: false`. An FPGA epoch admission result
returns `iq_valid: false, rx_epoch_valid: true`; only a host-validated first or
resumed META packet returns `iq_valid: true`. API success and timeout remain
distinct from data-validity proof.

## Verification

- Python extension rebuilt successfully against the current libbladeRF headers
  and library.
- `tests/test_rf_event_notifications.py`: 13 passed, including explicit
  validity mapping for transition intermediates and FPGA epoch admission.
- No hardware was required for this result-shape contract change.
