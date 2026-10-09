# Matched release RX_X2 production-rate 10,000-transition qualification

Date: 2026-10-09

## Setup

- xA4 on its already-running FPGA image; no FPGA reload or flash write during
  this run.
- Installed release-chain libbladeRF `2.6.1-git-58401a8a`.
- Native transition qualification harness, RX_X2 / BOTH, alternating
  947.5 MHz ↔ 1.835 GHz.
- 23.04 Msps, 32,768 samples per channel, 64 buffers / 32 transfers,
  6 s stream watchdog, 3 s transition timeout, close after each capture.

## Result

All 10,000 transitions returned a full paired block with matching certified
epoch/timestamp and ordered first-host-data confirmation. The retained event
history was complete from transaction 1 through 10,000; RX1/RX2 disable and
device close succeeded. Counts: unrecovered 0, first-read faults 0, recovered
reads 0, WOULD_BLOCK retries 0, retry overruns 0, stream overrun events 0.
Transition latency p50 21.813 ms, p95 22.440 ms, p99 23.145 ms, max
31.404 ms. A 32,768-sample/channel block completed in about 1.4 ms at the
configured sample rate.

The runtime collector counted 10,002 `RX_DATA_WITHHELD` notifications. They
were not accepted as IQ; every returned capture was accepted only after its
certified transition and timestamp checks. The two notifications beyond the
10,000 transitions have no per-event detail in this harness output and are
preserved here as an observability follow-up, not silently normalized.

Raw log: `docs/reports/rf/rx-epoch-matched-release-rxx2-10k-23msps-20261009.log`
(local ignored qualification output).

This closes the native RX_X2 transport/epoch 10,000-transition gate at the
production LTE sample rate and stream geometry. Production Python LTE repeat
qualification is recorded separately in the scanner release report.
