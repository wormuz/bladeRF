# Wishbone and RX reset sensitivity warnings

Date: 2026-10-09

## Finding and fix

Quartus A&S warning 10492 reported two processes that read signals absent from
their sensitivity lists:

- `bladerf_core.vhd` read asynchronous `rx_reset` in a process sensitive only
to `rx_clock`.
- `wishbone_master.vhd` tested `rising_edge(wb_clk_i)` in a process sensitive
to the Avalon `clock`, and reset the Wishbone FSM from the Avalon `reset`
although the component provides `wb_clk_i` and `wb_rst_i` for that domain.

The RX process now includes `rx_reset`. The Wishbone sequential process now
uses `wb_clk_i` and `wb_rst_i`. Both DCFIFOs declare the target family as
Cyclone V, matching the A4 design and allowing the official Quartus 25.1
`altera_mf` model to run in GHDL.

## Verification

- The existing Wishbone testbench now offsets the WB clock by 500 ps from the
  Avalon clock and has a 20 us watchdog. It completes at 14.694 us using the
  Quartus 25.1 `altera_mf.vhd` DCFIFO model. The transaction procedures wait
  for their Avalon read acknowledgments; the watchdog fails a stalled test.
- `python3 hdl/quartus/qcheck`: clean.
- `git diff --check`: clean.
- Fresh Quartus 25.1 sweep Analysis & Synthesis (`quartus_map --read_settings_files=on --write_settings_files=off bladerf -c sweep`): successful, 0 errors, 108 warnings. The prior report had 110; both 10492 sensitivity-list warnings are absent from the new map report.
- This was Analysis & Synthesis only. No fitter, assembler, RBF generation,
  FPGA load, or hardware operation was performed.

The test validates simulation wakeup and command/read-ack progress with
independent clock phases; it does not qualify post-fit timing or live board
behavior.
