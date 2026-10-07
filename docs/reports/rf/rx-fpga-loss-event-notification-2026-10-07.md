# FPGA RX loss episodes reach the event stream

Date: 2026-10-07

## Finding

The FPGA maintains a monotonic RX loss-event counter for episodes where its
sample writer discards input, and libbladeRF already exposed an explicit API
to read that counter. Nothing sampled it automatically, so the application
could receive IQ without an event when the FPGA dropped data but the USB queue
and metadata path did not independently report a discontinuity. The counter
measures loss episodes, not the number of samples lost.

## Change

The existing 100 ms RX integrity monitor now reads the FPGA RX loss-event
counter while an epoch is certified. On an increase it appends
`BLADERF_RF_EVT_RX_STREAM_OVERRUN` with
`BLADERF_RF_STREAM_STATUS_FPGA_RX_LOSS`; `rfic_status` carries the low 32 bits
of the cumulative counter, and `bladerf_get_loss_event_count()` remains the
authoritative full-width value. The Python wrapper identifies the source as
`fpga_rx_loss_counter` and keeps `iq_valid=False` for the event.

The monitor also marks the next synchronous RX call with
`BLADERF_META_STATUS_OVERRUN`. It does not revoke the RF epoch: this event
proves that the capture has a gap, while later samples may still belong to the
same valid RF configuration. A failed counter read or an unexpected counter
reset revokes the epoch instead of silently disabling loss monitoring.

## Verification

- Production `libbladeRF.so` build and RX transition policy test passed.
- Test-build xA4 monitor injection passed for RX1, RX2, and paired RX_X2. For
  each layout, the test observed the source-specific overrun event, received
  `BLADERF_META_STATUS_OVERRUN` on sync RX, and then received correctly
  epoch-tagged IQ. The injected increment exercises the monitor/event path;
  it does not electrically force an FPGA FIFO overflow.
- Python Cython extension build passed; RF event notification tests passed
  (11 tests), including the FPGA loss source and invalid-IQ semantics.

The hardware loss counter itself is read live from the connected FPGA. This
test does not qualify the FPGA's physical overflow detector or measure the
number of samples discarded during a real overflow episode.
