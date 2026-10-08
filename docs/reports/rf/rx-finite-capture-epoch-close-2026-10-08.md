# Finite RX capture closes its FPGA epoch

## Finding

Four LTE RX_X2 captures at PCI 85 were copied and passed epoch, timestamp,
sample-count, and paired-lane provenance checks. The app then decoded the
already copied capture while continuous RX continued. With no sync consumer,
the native sync ring eventually filled and libbladeRF correctly reported
`RX_STREAM_OVERRUN(SYNC_RX_RING_FULL)`. This was a queue lifecycle gap: a
finite-capture caller had no chain-level way to tell FPGA/libbladeRF that it
had finished consuming the current epoch.

## Change

`bladerf_rx_capture_close(dev, ch)` closes a certified finite RX epoch.
libbladeRF serializes against transitions and RF setters, revokes async and
sync IQ admission, invalidates queued sync samples, and sends the existing
NIOS RX epoch ABORT command. The FPGA gate then suppresses ADC IQ until the
next explicit transition arms a new epoch. AD9361 stays in RX; no LO or RFIC
configuration changes are made. On acknowledged close, the durable event
`BLADERF_RF_EVT_RX_CAPTURE_CLOSED` names the closed epoch. An abort error is
reported as `RX_EPOCH_ABORT_FAILED`, with the host certificate already
revoked.

The Cython wrapper exposes `pybladerf_rx_capture_close()`. Both single-lane
and RX_X2 LTE finite-capture paths call it immediately after copying and
validating IQ provenance, before CPU-heavy PSS/MIB/SIB decoding. StreamSession
revokes its old capture certificate on the close event.

## Repeatable local build and tests

The daily wrapper mismatch came from two independent defaults: setup.py used
system pkg-config headers/library, while `sdrscanner` preloaded system
libbladeRF globally before importing the Cython extension. The wrapper build
now prefers the sibling bladeRF fork checkout when available, including an
absolute RUNPATH. `sdrscanner` likewise preloads that same local fork first.
Explicit `PYTHON_BLADERF_CFLAGS` and `PYTHON_BLADERF_LDFLAGS` still override
the local-development defaults. This removes manual environment-variable
selection from ordinary local builds/tests.

## Verification

- bladeRF production library rebuilt; native sync epoch traversal test exits
  0 (its expected timeout-path diagnostic lines remain on stderr).
- Python wrapper rebuilt with no manual include/library environment; import
  resolves `libbladeRF.so.2` from the sibling fork and exposes the new method.
- python-bladerf suite: 29 passed.
- sdr-scanner `tests/driver`, `tests/hs_sweep`, and LTE dispatch tests: 73
  passed.
- Live xA4 RX1 finite capture returned valid host IQ, closed the epoch, and
  remained free of ring-full events during a 1.5 s decode pause. This confirms
  the close/ABORT path suppresses continuous-capture queue growth for that
  trial.
- The immediate next epoch did not qualify: `RX_EPOCH_VALID` and
  `RX_FIRST_VALID_HOST_DATA` were observed, followed by sync ring-full and a
  timestamp-discontinuity/withheld sequence with an older timestamp appearing
  after a later resumed timestamp. The close API therefore fixes pause
  accumulation in the observed trial, but close→rearm native sync ordering
  remains defective. Do not claim a stable release until this is corrected
  and repeated on hardware.
- Hardware also reported FPGA `v16.1.0` newer than the compatibility table,
  plus missing FPGA-size and VCTCXO trim calibration metadata. These warnings
  are environment discrepancies; no FPGA reload/reset was done during rearm
  investigation.
