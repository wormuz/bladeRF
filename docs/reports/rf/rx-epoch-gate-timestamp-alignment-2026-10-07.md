# Keep RX sample timestamps aligned through the epoch gate

The RX epoch gate registers sample controls and IQ before forwarding them to
`fifo_writer`. The writer previously received the live `rx_timestamp` counter,
which was not registered alongside that sample. This allowed META timestamps
to describe a later RX clock than the gated IQ; the epoch gate's
`first_valid_timestamp` was captured at the sample boundary, so the two
validity coordinates could disagree by a pipeline cycle.

Added a registered `out_timestamp` to `rx_epoch_gate` and connected that signal
to the RX `fifo_writer`. The timestamp now passes through the same register
stage as IQ, including for the paired RX1+RX2 path. The gate testbench checks
that both enabled lane samples and the timestamp have the same first-valid
boundary value.

Validation:

- `host/misc/run_rx_epoch_gate_tb.sh` — PASS: epoch gate, CDC handshake, FIFO
  epoch fence, abort and enable testbenches.
- `ghdl --synth --std=08 rx_epoch_gate` — PASS for the changed gate entity.
- `git diff --check` — PASS.
- Full Quartus synthesis was not run: `quartus_sh` is unavailable in this
  environment. The platform RX port association was checked against the sole
  `rx_epoch_gate` instantiation and the `fifo_writer` timestamp input.

No timeout or fixed discard establishes IQ validity.
