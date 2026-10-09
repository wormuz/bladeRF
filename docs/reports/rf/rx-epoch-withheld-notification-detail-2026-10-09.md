# RX_X2 withheld-event detail follow-up

Date: 2026-10-09

The native RX_X2 production-rate 10,000-transition qualification was repeated
with full fields emitted for every `RX_DATA_WITHHELD` event. The run used the
installed `2.6.1-git-58401a8a`, 23.04 Msps, 32,768 samples/channel, 64 buffers,
32 transfers, paired 947.5 MHz / 1.835 GHz transitions, and terminal capture
close after each block.

All 10,000 paired captures completed with matching certified epoch/timestamp
and host-data confirmation. There were zero read faults, retries, short reads,
or stream overruns. Transition latency was P50 21.807 ms, P95 22.496 ms,
P99 23.189 ms, maximum 38.789 ms.

The event collector recorded 10,005 `RX_DATA_WITHHELD` notifications. Every
one had `flags=0x10000001` (epoch uncertified plus valid transition-channel context),
`transaction_id=0`, `fpga_state=RX_DATA_INVALID`, and no FPGA timestamp. Five
capture boundaries produced a second notification for the same 8-bit epoch;
in each case both events appeared after `RX_CAPTURE_CLOSED`, and that epoch
was the just-closed/preceding epoch. No IQ from these intervals was accepted.
This is consistent with delayed old-epoch USB callbacks being reported after
capture close, but the event currently lacks source packet metadata and a
transaction ID, so callback provenance is not yet proven.

The successful 10k transport/epoch gate remains valid. The remaining
observability defect is precise: a withheld event cannot identify its
transaction/source epoch, and five boundaries emitted a duplicate old-epoch
notification. Instrument the callback source with packet metadata and correlate
events to capture-close sequence before changing notification deduplication.

Raw qualification log (local):
`docs/reports/rf/rx-epoch-withheld-detail-rxx2-10k-20261009.log`.
