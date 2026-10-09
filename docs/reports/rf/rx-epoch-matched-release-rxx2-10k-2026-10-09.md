# Matched release RX_X2 10,000-transition qualification

Date: 2026-10-09

## Setup

- xA4 using its already-running FPGA image and installed release-chain
  libbladeRF `2.6.1-git-58401a8a`; no FPGA reload or flash write was performed
  during this qualification.
- Native `host/misc/run_rx_epoch_transition_qualification.sh` using the same
  locally built library as the installed system library.
- RX_X2 / BOTH, alternating 947.5 MHz ↔ 1.835 GHz, 4 Msps, 32,768 samples
  per channel, stream ring 64 buffers / 32 transfers, 6 s stream watchdog,
  3 s transition timeout, capture-close after every block.

## Result

The harness completed all 10,000 transitions. It returned 10,000 full paired
captures with matching certified epoch and timestamp, complete transition
history from transaction 1 through 10,000, and successful RX1/RX2 disable and
device close. Counts: unrecovered transitions 0, first-read faults 0,
recovered reads 0, `WOULD_BLOCK` retries 0, capture overruns 0, and stream
overrun events 0. Transition latency was p50 24.332 ms, p95 28.198 ms, p99
28.496 ms, max 40.249 ms.

The event collector also observed 10,000 `RX_DATA_WITHHELD` notifications,
one per transition interval. The epoch gate intentionally rejects data while
the new epoch is uncertified; the harness only accepts a full block after
`RX_EPOCH_VALID` and `RX_FIRST_VALID_HOST_DATA` are confirmed in the ordered
transaction history. These notifications are therefore the explicit
fail-closed boundary, not accepted IQ or a transport overrun.

Raw log: `docs/reports/rf/rx-epoch-matched-release-rxx2-10k-crossband-20261009.log`
(local ignored qualification output).

This closes the current native RX_X2 transport/epoch 10,000-transition gate
for this geometry. Python end-to-end LTE repeatability and the independent
no-PSS RF-content question remain separate release gates.
