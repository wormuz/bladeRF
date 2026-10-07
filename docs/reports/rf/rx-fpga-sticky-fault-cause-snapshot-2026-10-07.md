# FPGA RX fault cause notification through libbladeRF

Date: 2026-10-07

## Finding

The RX validity monitor already failed closed when the FPGA's aggregate sticky
RX fault bit asserted. The FPGA writer tracks five causes, but firmware exposed
only `rf_link_status[14]`. Consequently libbladeRF and Python could announce
that IQ had become invalid, but could not tell whether the writer saw a speed
mismatch, no progress after start, GPIF timeout, protocol error, or FIFO abort.

## Change

The micro FPGA now transfers all five sticky cause bits from `rx_clock` to
`sys_clock` through a bundled-data handshake and publishes them as a read-only
NIOS PIO at `0x9680`. NIOS target `0x8c` returns the coherent 32-bit snapshot.
The host fault monitor reads it only after the aggregate fault is asserted,
while holding the existing device lock. It always invalidates the certified RX
epoch as before; when a supported FPGA image supplies causes, the event also
carries a validity marker and cause mask. Older images preserve the aggregate
status behavior.

The Python event dictionary keeps `iq_valid: false` and adds
`fpga_rx_fault_causes`, a list of decoded cause names when the snapshot marker
is present. RX1, RX2, and shared RX_X2 use the same fabric cause word and
invalidation path; the causes are not channel-specific.

## Verification

- Updated Qsys generation succeeded; generated NIOS `system.h` maps the PIO at
  `0x9680` with a 32-bit input.
- NIOS BSP and firmware build succeeded with `-Werror`; the new NIOS read
  target compiled and a fresh 93 KiB ELF plus memory initialization files were
  generated.
- Quartus xA4 synthesis and assembly passed in the isolated project after
  regenerating the NIOS firmware image (0 errors; 134 warnings).
- `libbladerf_shared` and native sync/validity tests passed.
- Python extension build passed and RF event tests passed (13 tests), including
  decoding simultaneous GPIF-timeout and FIFO-abort causes.
- The existing RX epoch gate testbench passes for single-lane and paired
  RX_X2 behavior.
- The consolidated `host/misc/run_rx_epoch_gate_tb.sh` now also runs the
  asynchronous 32-bit handshake re-arm test used by the coherent cause
  snapshot; the complete runner passed.

## Remaining hardware check

No physical GPIF/FIFO fault was induced on the board in this run. The cause
transport is compile/synthesis verified, but the five decoded causes still need
on-board fault injection or naturally occurring fault capture to qualify their
bit-to-event mapping. The existing aggregate fail-closed notification remains
active if a snapshot read fails or an older FPGA image is loaded.
