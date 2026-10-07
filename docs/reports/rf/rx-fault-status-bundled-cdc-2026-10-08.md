# RX FIFO fault status bundled CDC check (2026-10-08)

## Finding

The latest full xA4 Quartus compile reports eight D103 nodes in the RX and TX
FIFO-writer sticky `fault_sticky_i` vectors. The vectors are not direct NIOS
inputs: RX fault causes are packed in the `rx_clock` domain and cross through
`U_rx_fault_causes_handshake`, then captured in the `sys_clock` domain for the
NIOS PIO. The bundled-data path is constrained in the platform SDC. The sticky
vector is used to preserve simultaneous causes as one coherent status word;
independent bit synchronizers would weaken that snapshot contract.

## Validation added

`hdl/fpga/ip/nuand/simulation/rx_fault_causes_cdc_tb.vhd` instantiates the
production `handshake` entity with independent RX/system clocks and the same
request/acknowledge/capture pattern used for the NIOS status PIO. It verifies
that simultaneous causes arrive as one vector, a source update cannot corrupt
an in-flight snapshot, the following poll includes newly sticky bits, and a
new-epoch clear arrives coherently.

Results:

- `hdl/quartus/qsim rx_fault_causes_cdc_tb`: pass.
- `hdl/quartus/qcheck`: clean; all 15 constrained handshake pairs remain
  structurally matched to clock crossings.
- `git diff --check`: clean.

This is a protocol simulation and structural constraint check, not a new
Quartus compile or hardware fault injection. The D103 records remain visible;
no warning was waived, and project-wide `qgate` remains open on the remaining
critical warnings. The test supplies evidence for classifying this particular
status path, not for declaring the whole CDC audit complete.
