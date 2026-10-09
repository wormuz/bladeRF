# NIOS AD9361 AXI timeout semantics audit — 2026-10-09

## Finding

The preserved Host→FPGA/NIOS nonresponse was initially attributed to an
unbounded Avalon waitrequest when the AD9361 interface clock is absent. That
description does not match the checked-in hardware topology or ADI adapter.

- `nios_system.tcl` connects `system_clock.clk` to
  `axi_ad9361_0.s_axi_clock`.
- `axi_ad9361_hw.tcl` associates the AXI-Lite slave with `s_axi_clock`.
- `axi_ad9361.v` connects `up_axi.up_clk` to `s_axi_aclk`.
- `up_axi.v` has independent read and write response counters. If the core
  does not acknowledge, each adapter forces a response after its counter
  reaches `5'h1f`, around 15 cycles after request acceptance.
- On read timeout, `up_rdata_s` is `0xDEADDEAD`. The AXI `rresp` remains OKAY.
  On write timeout, the adapter synthesizes an acknowledgment and its AXI
  `bresp` remains OKAY. Firmware receives no explicit timeout indication.

Thus a stopped `if_l_clk` does not, by itself, explain an indefinitely blocked
NIOS Avalon instruction through this control path. It may produce invalid or
ignored ADI register accesses; the current no-response may instead occur in a
later polling loop, another peripheral, or packet servicing. The preserved
run captured no NIOS program counter or live init stage, so root cause remains
open.

## Related changes and disposition

The FPGA clock monitor and NIOS preflight remain in the source as a data-path
readiness check. They are not an AXI waitrequest timeout or a demonstrated
fix for the hang. Corrected comments in `devices_rfic_cmds.c` now say this
explicitly. No board operation was performed.

The concrete stack gap is silent timeout reporting: AXI read timeouts return
a sentinel without an error response, and AXI write timeouts look successful.
The NIOS/no-OS path must propagate those outcomes as an RFIC initialization
failure. That does not yet explain the lost response and needs a separate
implementation/verification pass. The FX3 vendor status path still reads
only FPGA `CONFDONE`; the board exposes no separate NIOS heartbeat input to
FX3.

Evidence: `nios_system.tcl` connections around lines 739–749;
`axi_ad9361_hw.tcl` AXI interface declaration; `axi_ad9361.v` `i_up_axi`
connection; `hdl/fpga/ip/analogdevicesinc/hdl/library/common/up_axi.v` read
and write response counters at lines 174–204 and 234–264; preserved USB
failure record `nios-mode-switch-no-response-2026-10-09.log`.

## Qsys response-path follow-up — 2026-10-09

Inspected the generated `nios_system.sopcinfo` and synthesis HDL from the
qualified sweep A4 build, rather than inferring behavior from the `.tcl`
connection alone. The NIOS II `data_master` is an Avalon-MM master, while
`axi_ad9361_0.s_axi` is AXI4-Lite; Qsys inserts a Merlin protocol path between
them. The NIOS II CPU wrapper exposes only `d_waitrequest`, `d_readdata`, and
`d_readdatavalid` for data transactions; it has no response/error input. In
the generated `altera_merlin_master_translator` for this master,
`uav_response` is tied to `2'b00`, `av_response` is unconnected, and
`uav_writeresponsevalid` is tied low. Therefore the ADI AXI `BRESP` cannot be
observed by NIOS software through `IORD/IOWR`; changing `BRESP` from OKAY to
SLVERR alone does not provide a firmware status path.

This resolves the bridge uncertainty and rules out a BRESP-only fix. A
software-visible sideband/sticky timeout status is required (or an equivalent
Qsys-supported error channel that reaches NIOS software). It must be
qualified end to end before `adi_axi_write()` can return failure. No RTL or
image was changed/loaded in this audit. Root cause of the preserved NIOS
no-response remains open.

Generated evidence: `hdl/quartus/builds/000075-sweepxA4-seed7-7475fc3/src/hdl/quartus/work/bladerf-micro-A4-sweep/nios_system/synthesis/submodules/nios_system_nios2.v`
(CPU Avalon ports); same directory's `nios_system_mm_interconnect_0.v`
(AXI4-Lite agent and NIOS master translator); and
`nios_system.sopcinfo` (master/slave protocol types and address map). These
are generated outputs and are not source-of-truth inputs to edit.
