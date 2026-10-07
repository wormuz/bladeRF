# RX epoch controller CDC and synthesis regression — 2026-10-07

This change extracts the production NIOS `sys_clock` to RX `rx_clock`
handshake and command-toggle decode into `rx_epoch_controller.vhd`. The
32-bit bundled control word continues to carry ARM/COMPLETE/ABORT toggles,
epoch ID, and metadata enable together. The production `bladerf_core` now
instantiates that entity, and every xA4 RX project QIP includes it.

## Verification

- `host/misc/run_rx_epoch_gate_tb.sh`: passed. This runs the production
  controller with the real bundled `handshake` and `rx_epoch_gate`; it checks
  initial ARM/COMPLETE, paired RX_X2 sample admission at the first-valid
  timestamp, ABORT fencing both IQ lanes, and recovery into a new epoch ID.
- `hdl/quartus/qcheck`: clean.
- `git diff --check`: clean.
- Quartus Prime 25.1 Standard synthesis, bladeRF-micro A4 `sweep`, seed 5:
  Analysis & Synthesis successful, 0 errors, 108 warnings. The log confirms
  `rx_epoch_controller` elaborated under `bladerf_core:U_core`. This was a
  synthesis flow only; no fitter or programming image was produced.

The first incremental attempt used an old generated NIOS/Qsys system and
failed because it lacked the already-required `rx_fault_causes_export` PIO.
Regenerating Qsys from the checked-in platform Tcl resolved that stale-build
input; the regenerated project then synthesized successfully. No RTL change
was made to hide or bypass that port mismatch.

This regression establishes control-path integration and the two-lane
sample-boundary behavior in simulation. It does not establish RFIC lock,
USB delivery, or hardware transition latency; the attached board could not
be opened by libbladeRF during this session, so no live qualification or
FPGA programming was performed.
