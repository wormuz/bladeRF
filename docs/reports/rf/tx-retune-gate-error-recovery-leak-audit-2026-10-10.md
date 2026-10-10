# TX retune gate ERROR recovery leak

The FPGA TX retune gate had a fail-open recovery path. After ABORT or FIFO
underflow, the gate entered `STATE_ERROR` and correctly replaced DAC IQ with
zero. A later ARM carrying a future timestamp changed the state to
`STATE_ARMED`; before that timestamp, combinational `muted_i` became false.
That allowed queued, pre-fault IQ to reach the DAC while RFIC recovery was
still unconfirmed.

`STATE_ERROR` now remains muted immediately when a fresh ARM arrives. The gate
can leave `STATE_MUTED` only after a later COMPLETE pulse. The source
testbench's underflow-recovery case now checks the future-timestamp ARM stays
muted and only COMPLETE resumes nonzero IQ.

This is a source correction and testbench regression only. No simulation,
build, Quartus synthesis, image load, or hardware test was run under the active
source-audit gate. The updated testbench still needs simulation after the
complete RX+TX source audit finds and fixes the remaining defects.
