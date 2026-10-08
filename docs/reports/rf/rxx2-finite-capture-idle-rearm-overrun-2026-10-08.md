# RX_X2 finite capture followed by idle rearm overrun — 2026-10-08

## Finding

The high-throughput finite-capture lifecycle still fails on the xA4 even
though the earlier small-buffer close/rearm qualification passed. With paired
RX_X2, 23.04 MS/s, 32,768 samples per stream buffer, 3,456,000 samples per
lane (150 ms), and a 250 ms pause after `bladerf_rx_capture_close()`, the
second transition is reported complete but its synchronous capture returns
only 1,013,824 of 3,456,000 samples. META reports `0x30001`; four native
`RX_STREAM_OVERRUN` events are recorded (`0x10000045` and `0x10000001`). The
read call itself returns success, so callers must inspect the META validity
status and the native event history; they must not infer a complete capture
from the function return alone.

Reproduction command:

```sh
BLADERF_QUAL_STREAM_BUFFER_SAMPLES=32768 \
BLADERF_QUAL_CAPTURE_SAMPLES=3456000 \
BLADERF_QUAL_SAMPLE_RATE=23040000 \
host/misc/run_rx_epoch_transition_qualification.sh \
  2 BOTH --cross-band --close-after-capture --close-pause-ms 250
```

The native run used the existing committed libbladeRF and current FPGA image;
it did not use Python, detector DSP, systemd, or a background service. The
first 150 ms capture completed in 150.099 ms. The second read returned
1,013,824 samples in 115.780 ms and raised four native overrun events. Full
raw event trace: `rxx2-long-capture-250ms-rearm-2026-10-08.log`.

The companion zero-pause run completed both 150 ms read calls without a
stream-overrun event, but the qualification harness did not accept either
capture's validity trace. It is therefore diagnostic only, not a pass. Raw
trace: `rxx2-long-capture-immediate-rearm-2026-10-08.log`.

The same 23.04 Msps / 150 ms / 250 ms close-pause profile passed two cycles
each on RX1 and RX2: exact full reads, zero first-read faults/retries, and zero
stream overruns. Their raw logs are `rx1-long-capture-250ms-rearm-2026-10-08.log`
and `rx2-long-capture-250ms-rearm-2026-10-08.log`. This narrows the reproduced
failure to paired RX_X2 under the tested stream geometry; it does not qualify
longer single-channel soaks or all RX_X2 configurations.

## Interpretation

`bladerf_rx_capture_close()` revokes host IQ admission and aborts the FPGA RX
epoch gate, but leaves the async worker running. The async META validator
rejects uncertified buffers and routes them through the sync worker's rejected
sequence path, so this evidence does **not** establish that invalid IQ simply
fills the ring during the pause. The overrun is reported during the next
certified epoch as `SYNC_RX_RING_FULL`. The sequence trace shows `sync_rx()`
skipping 3 then 92 rejected ring slots; the next callbacks are buffer 56 /
sequence 632 onward, with `expected_seq=632`. The ring then overruns at
sequence 697 while `cons_i=53` and `prod_i=57`, after which 64 stale buffers
are dropped. The first reported timestamp gap is expected 10,394,935 but
received 11,899,319. The exact interaction between rejected in-flight
transfers, sequence re-anchoring, and the high-throughput RX_X2 ring remains
to be isolated. The 250 ms idle interval is outside the 150 ms capture itself.
The configured 32 in-flight transfers represent roughly 1.05 million samples
per lane, close to the 1,013,824-sample prefix returned before the reported
gap, which makes transfer-tail ordering a specific hypothesis to test rather
than a proven cause. Detailed native trace:
`rxx2-long-capture-sequence-debug-2026-10-08.log`.

Separate Python diagnostics on the same capture profile measured about
742.8 ms for RX1 SC16 conversion, resampling, and PSS processing after capture
close. That provides a realistic interval for the native close/rearm defect to
surface. The failing condition is not evidence of LTE channel degradation.

## Disposition

Release blocker for the production RX_X2 sweep profile. Do not mark the chain
stable based on the earlier 1,000-cycle small-capture qualification. A first
attempt to stop and restart the async worker was not retained: it introduced
restart and transition failures and then a further RX_X2 ring overrun. The
next investigation must instrument transfer sequence IDs, metadata epoch IDs,
and timestamps across close, the 250 ms gap, and first new-epoch delivery; then
fix the native lifecycle while preserving FPGA epoch and host-data ordering.
Do not rely on Python discard counts.

Required regression after the fix:

1. Repeat 150 ms RX_X2 captures with 250 ms and 1 s closed-epoch processing
   gaps; require exact sample counts, valid META, and zero native overruns.
2. Repeat the same lifecycle on RX1 and RX2.
3. Run repeated cross-band transitions and the LTE known-cell return sweep.
4. Re-run the 1,000-cycle close/rearm matrix and full release checks.
