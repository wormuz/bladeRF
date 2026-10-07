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
notification on one xA4. It does not replace the longer 10,000-transition
latency/loss qualification or a physical fault-injection measurement for every
fault-cause bit.
