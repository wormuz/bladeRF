# AD9361 AXI timeout sideband RTL simulation

Date: 2026-10-09

## Result

The focused `up_axi` timeout testbench passes under Icarus Verilog 12.0.
Icarus was unpacked under `/tmp/iverilog-runtime` from the Ubuntu package;
no system package installation was performed.

Command:

```sh
/tmp/iverilog-runtime/usr/bin/iverilog -g2012 -Wall \
  -s tb_up_axi_timeout -o /tmp/up_axi_timeout.vvp \
  hdl/fpga/ip/analogdevicesinc/hdl/library/common/up_axi.v \
  hdl/fpga/ip/analogdevicesinc/hdl/library/common/tb_up_axi_timeout.v
/tmp/iverilog-runtime/usr/bin/vvp /tmp/up_axi_timeout.vvp
```

Observed result:

```text
up_axi timeout sideband: PASS
```

The bench checks a normal acknowledged write, a timed-out write returning
SLVERR and setting sticky timeout status, clear-toggle behavior, a timed-out
read returning `0xDEADDEAD` and SLVERR, a second clear, and a normal read
returning `0x12345678` with OKAY and no timeout indication.

## Testbench correction

The first execution failed because the testbench drove ACK before `up_axi`
had entered its ACK-wait counter phase. The design correctly timed out; the
bench's supposed success case was malformed. ACK scheduling now waits for the
request and one counter-arming edge before asserting ACK. The corrected test
passes for both read and write paths.

## Scope

This is a module-level functional test of the AXI adapter and its timeout
sideband. It does not replace the separately recorded Qsys integration and
Quartus A4 Analysis & Synthesis, nor does it prove the complete NIOS/USB
runtime path or hardware behavior. The NIOS and no-OS read/write error
propagation changes are in bladeRF commits `0a185127` and `af548f7e`; the
Qsys/FPGA sideband is in `0a185127`.
