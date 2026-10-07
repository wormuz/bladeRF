# Distinguish FPGA RX epoch validity from host IQ delivery

## Finding

The public RF transition overview said `RX_EPOCH_VALID` established
application-usable IQ. That event is emitted when the FPGA epoch gate opens at
the timestamped sample boundary. It does not prove that any META packet has
reached a host consumer. The header already defined
`BLADERF_RF_REQUIRE_FIRST_HOST_DATA` for that stronger condition, and the
Python wrapper already reported `RX_EPOCH_VALID` with `iq_valid=False` and
`rx_epoch_valid=True`.

## Change

Corrected the public header contract:

- `RX_EPOCH_VALID` / `BLADERF_RF_STATE_RX_DATA_VALID` means FPGA sample
  admission at the new epoch boundary.
- `RX_FIRST_VALID_HOST_DATA` proves that the first epoch- and
  timestamp-validated META IQ reached the host.
- `BLADERF_RF_REQUIRE_FIRST_HOST_DATA` makes transition wait require that
  host-delivery event; `BLADERF_RF_REQUIRE_RX_X2_HOST_DATA` requires a paired
  RX_X2 packet.

This is an ABI-neutral documentation correction. It adds no timer, discard,
or validity shortcut.

## Validation

- Compared public declarations and event semantics in
  `host/libraries/libbladeRF/include/libbladeRF.h` with the native event
  emission/admission code in `src/board/bladerf2/rf_transition.c` and
  `rx_event_history.c`.
- Compared the Python mapping in
  `python_bladerf/pylibbladerf/pybladerf.pyx`: epoch admission remains
  `iq_valid=False`; only validated first host data is `iq_valid=True`.
- `git diff --check` passes.
- No hardware result is claimed.
