# RFIC RSSI validity audit

Date: 2026-10-10

## Findings and source changes

`bladerf_get_rfic_rssi()` dispatched TX channels to `ad9361_txmon_enable()` and
returned a numeric value even though both bladeRF 2.0 micro TX_MON inputs are
documented as grounded in `fpga_common/src/ad936x_params.c`. ADI's AD9364
reference manual describes the TX power monitor as measuring a signal brought
to the TX_MON input through an external coupler. The returned number therefore
did not certify transmitter output power, and the query also changed RFIC state.
The public bladeRF2 RSSI API now rejects malformed channels and reports TX RSSI
as `BLADERF_ERR_UNSUPPORTED`; RX1/RX2 remain supported.

In FPGA command mode, the reply's RSSI multiplier was used as a divisor without
checking for zero. A zero or malformed reply could therefore produce a
non-finite value and reach integer rounding. The command-mode decoder now
returns `BLADERF_ERR_UNEXPECTED` before writing either output unless the
multiplier is positive.

The source regression policy covers RX1/RX2 support, TX unsupported, malformed
channels, and zero/negative multipliers. No build or test was run under the
active source-audit gate. This does not close the broader RX+TX audit.

## References

- `fpga_common/src/ad936x_params.c`: bladeRF micro TX_MON1/TX_MON2 are tied to
  ground.
- [ADI AD9364 Reference Manual UG-673, Tx Power Monitor](https://ez.analog.com/cfs-file/__key/telligent-evolution-components-attachments/00-441-00-00-00-08-03-45/AD9364_5F00_Reference_5F00_Manual_5F00_UG_2D00_673.pdf): describes the detector as measuring the transmitter through the TX_MON input and external coupling path.
