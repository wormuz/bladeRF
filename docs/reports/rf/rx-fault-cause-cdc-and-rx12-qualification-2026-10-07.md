# RX fault-cause CDC and RX1/RX2 qualification — 2026-10-07

## Changes

- Added `U_rx_fault_causes_handshake` to the bladeRF Micro bundled-data SDC
  pairs. The FPGA now reports 14 constrained handshake crossings, including
  the fault-cause snapshot bus.
- Fixed FPGA reload initialization: `bladerf_load_fpga()` holds the RX
  reconfiguration reservation across image replacement and RFIC reinit. The
  internal PLL setup now reuses that reservation instead of recursively
  entering the public invalidation path and returning `BLADERF_ERR_WOULD_BLOCK`.
- Added setup-path diagnostic script `hdl/quartus/report_setup_diag.tcl`.

## Build and timing

Full Quartus flow for xA4 completed successfully in the isolated
`adr0207-rx-cause-full-xa4` project on 2026-10-07. Synthesis, fitter, STA, and
assembler reported zero errors. STA reported 14 handshake crossings
constrained, worst slow-corner setup slack +0.569 ns, and worst hold slack
+0.199 ns. The generated RBF is 2,632,660 bytes, SHA-256
`a6d9adcb2fea943cdbef42b88a40f247b6790a7ccea4b2d7e50f95e36823a1ac`.

## Hardware qualification

The generated RBF was loaded into the xA4 without writing flash. The board's
flash FPGA-size query returns `BLADERF_ERR_INVAL`, so loading used the known
xA4 override (`BLADERF_FORCE_FPGA_A4=1`) and local-image size-check override
(`BLADERF_SKIP_FPGA_SIZE_CHECK=1`). The FPGA reload and subsequent RFIC
initialization completed successfully after the reservation fix.

`host/misc/run_rx_transition_validity_live.sh` passed in all three modes:

- `RX1`: single-channel transition and IQ validity fencing passed.
- `RX2`: second-channel transition and IQ validity fencing passed.
- `BOTH`: RX1 and RX2 enabled together in RX_X2; transition and paired-stream
  validity fencing passed.

Each run also confirmed that legacy frequency, bandwidth, sample-rate, TX FIR,
configuration GPIO, and feature changes revoke the previous RX data-valid
certificate until a successful event-driven transition creates a fresh epoch.

## Remaining qualification

This validates channel selection, paired streaming, FPGA reload, and validity
notification on one xA4. A new `host/misc/run_rx_epoch_transition_qualification.sh`
qualification now runs event-order, stale-epoch filtering, completion, and
runtime-event checks across RX1, RX2, and RX_X2. It drains the global event
history by sequence, so transaction ID 0 overrun/withheld notifications are
included and history loss fails the run.

On 2026-10-07, 3,334 transitions per mode completed with no unrecovered IQ
read failures:

| Mode | P50 / P95 / P99 transition latency | Unrecovered | Runtime overrun events | Withheld notifications |
|---|---:|---:|---:|---:|
| RX1 | 6.909 / 7.201 / 7.427 ms | 0 | 0 | 3,334 |
| RX2 | 6.909 / 7.207 / 7.671 ms | 0 | 0 | 3,334 |
| RX_X2 | 6.913 / 7.205 / 7.431 ms | 0 | 7 | 3,337 |

All three modes returned `WOULD_BLOCK` on the first sync read after every
transition, then recovered on a later read (3,334/3,334 in each mode). This
is the expected fail-closed behavior while the next valid host packet has not
arrived. Each transition also emitted its runtime data-withheld notification.
The seven RX_X2 overrun events are real qualification failures even though
the sample reads recovered; their flags were `BLADERF_RF_STREAM_STATUS_OVERRUN`
without `BLADERF_RF_STREAM_STATUS_FPGA_RX_LOSS`, so they identify a host
stream/queue discontinuity rather than an FPGA sample-path loss. The current
harness now exits nonzero if any such event occurs. A separate 1,000-transition
RX_X2 rerun saw two overrun events on the first transition (epoch 1); a later
100-transition rerun saw two on its first transition (epoch 101), and another
100-transition rerun saw none. This is intermittent and needs follow-up
before claiming zero-loss RX_X2 qualification. The public event currently
does not identify whether a non-FPGA overrun came from sync queue accounting,
USB short/overflow, or timestamp discontinuity; this is a separate event
diagnostic gap to close.

The aggregate run covers 10,002 transitions, but it does not replace physical
fault injection for every FPGA fault-cause bit or a larger repeated RX_X2
zero-overrun qualification.
