# Dwell readout clock-domain crossing

## Finding

In the sweep FPGA revision, `dwell_readout.rd_data` was generated in
`rx_clock` and connected directly to the NIOS/system-clocked Avalon PIO. The
source register did not make this a safe clock crossing. Quartus DRC reported
the path as a cross-domain data structure, and the old SDC did not describe a
handshake for the NIOS readout.

The same audit exposed two verification defects: the RX epoch handshake SDC
pair still named a removed instance, and `qcheck` stopped parsing the SDC list
at the `]` inside a literal `[*]` wildcard, so it missed later pairs.

## Change

- Added a request/acknowledge bundled-data handshake from the RX-clocked
  selected dwell word to a system-clocked register which drives the PIO.
- Added the source-holding to destination-register timing bounds to the SDC.
- Corrected the RX epoch handshake hierarchy to the synthesized
  `rx_epoch_controller:U_rx_epoch_controller|handshake:transfer` path.
- Fixed `qcheck` to parse the complete wildcard list and validate named
  handshake entities as well as `U_*` instances.
- Updated the sweep handshake inventory from 14 to 15 and added an independent
  clock GHDL test for the dwell readout crossing.

## Validation

- `qcheck`: clean.
- `qsim dwell_readout_cdc_tb`: 1 case passed with unrelated 7 ns and 11 ns
  clock periods.
- `qsim dwell_readout_tb`: all 9 cases passed.
- Full Quartus Prime 25.1std compile, Cyclone V 5CEBA4F23C8 sweep revision:
  0 errors, 272 warnings. Final SDC processing reports 15 constrained
  handshake crossings and 2 readout crossings. Setup slack is nonnegative,
  with a minimum of +0.088 ns; hold slack minimum is +0.188 ns.
- The DRC no longer reports the old direct `dwell_readout.rd_data` path.
  The standard bundled-data handshake source is still listed by Quartus D101;
  its held source bus, request/acknowledge protocol, and explicit
  max-skew/net-delay bounds are present in RTL and SDC.
- `qgate` reports the successful build and timing, but exits with 43
  Critical Warning diagnostics across the design, including legacy ADI/CDC
  findings. These were not waived. A project-wide DRC cleanup remains open.
- No bitstream was loaded and no board-level sweep or RF qualification is
  claimed.

The full compile log and generated reports are in the ignored Quartus work
directory `hdl/quartus/work/adr0207-rx-cause-full-xa4/output_files/`.
