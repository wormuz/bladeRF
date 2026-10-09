# Exact release-candidate RX_X2 10,000-transition qualification

Date: 2026-10-09

## Artifact identity

- Installed and bundled libbladeRF: `2.6.1-git-3c6466af`.
- SHA-256: `e61df9d53fbec1f988dac5255ff9476f20a274c4f484bebb175c573d09f6825a`.
- The native harness linked the same local build; its RPATH resolved
  `host/build/output/libbladeRF.so`, byte-identical to the installed library.
- xA4 serial `f695006ba84a40daa7b777c6c6eba78`; FPGA image remained the
  previously loaded, qualified build-000074. No FPGA reload or flash write.

## Conditions and result

- RX_X2 / BOTH, alternating 947.5 MHz and 1.835 GHz.
- 23.04 Msps, 32,768 samples/channel, 64 buffers / 32 active transfers.
- 10,000 transitions, terminal capture close after every paired capture.
- 10,000/10,000 full captures with matching epoch/timestamp and ordered
  first-host-data confirmation; complete retained event history 1–10000.
- Zero unrecovered transitions, first-read faults, recovered reads,
  WOULD_BLOCK retries, retry overruns, or stream overrun events.
- RX1/RX2 disable and device close completed successfully.
- Transition latency P50 21.807 ms, P95 22.433 ms, P99 23.103 ms,
  maximum 31.465 ms.

The runtime collector saw 10,002 `RX_DATA_WITHHELD` events. The source epoch
and FPGA timestamp were present. Two events repeated a preceding source epoch
after capture close, at distinct timestamps 98,112 samples apart. They are
delayed USB buffers from the old epoch; each was withheld and no IQ from them
was returned. Their `transaction_id` remains zero, which is an observability
limitation only.

Raw local qualification trace:
`docs/reports/rf/rx-epoch-release-candidate-rxx2-10k-20261009.log`.
