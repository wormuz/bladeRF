# RX epoch ARM-edge stale sample

In `STATE_ACTIVE`, `rx_epoch_gate` previously changed the internal state to
`STATE_PENDING` on ARM but still forwarded the sample present on that same
clock edge. The host had already revoked its epoch certificate, and this
sample retained the old epoch tag, so the reviewed path did not establish an
application-visible stale-IQ leak. It nevertheless allowed one unnecessary
old-epoch sample into the RX FIFO after the hardware fence command reached the
sample domain.

The gate now emits zero-IQ with the input `data_v` preserved on the ARM edge.
It keeps stream controls enabled and the usual keepalive behavior, while
preventing the extra old sample from entering the FIFO. The VHDL regression
source checks both lanes on the ARM edge.

This is source/testbench work only. No simulation, build, Quartus synthesis,
image load, or hardware operation was run under the active full source-audit
gate. Simulation remains required after the RX+TX defect sweep is complete.
