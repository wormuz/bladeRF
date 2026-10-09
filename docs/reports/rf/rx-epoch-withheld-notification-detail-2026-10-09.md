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

The first detailed run recorded 10,005 `RX_DATA_WITHHELD` notifications, all
with `transaction_id=0` and no FPGA timestamp. Five boundaries had a second
notification for the same epoch, but the generic event did not identify the
source buffer.

## Source-metadata follow-up

The async admission path now preserves the source packet's epoch tag and
timestamp when it withholds a META buffer during an uncertified interval. It
still rejects the buffer before application delivery. The rebuilt local
library passed the sync/epoch native traversal regression and a 100-transition
live xA4 check; all 100 captures completed, and each withheld event carried
source epoch and FPGA timestamp.

A new 10,000-transition live run completed all paired captures with zero read
faults, retries, or overruns (P50 21.807 ms, P95 22.442 ms, P99 23.179 ms,
max 36.721 ms). It emitted 10,003 withheld notifications. Three duplicate
source epochs had two distinct timestamps separated by 16,352 samples. Both
notifications arrived after `RX_CAPTURE_CLOSED` for the next epoch, while
their source tags still identified the preceding epoch. This proves these
notifications represent delayed old-epoch USB buffers, not repeated
invalidation of the current epoch. All were withheld; no stale IQ reached the
caller.

The source epoch/timestamp are now observable, while `transaction_id` remains
zero for old-epoch buffers because the callback does not retain a transaction
mapping for the source packet. The remaining follow-up is to correlate those
packet tags to retained epoch-valid events or carry transaction provenance in
the FPGA metadata. Notification deduplication is not needed for correctness.

The successful 10k transport/epoch gate remains valid. The remaining
observability defect is precise: a withheld event cannot identify its
transaction/source epoch, and five boundaries emitted a duplicate old-epoch
notification. Instrument the callback source with packet metadata and correlate
events to capture-close sequence before changing notification deduplication.

Raw qualification logs (local):
`docs/reports/rf/rx-epoch-withheld-detail-rxx2-10k-20261009.log`,
`docs/reports/rf/rx-epoch-withheld-source-live-100-20261009.log`,
`docs/reports/rf/rx-epoch-withheld-source-rxx2-10k-20261009.log`.
