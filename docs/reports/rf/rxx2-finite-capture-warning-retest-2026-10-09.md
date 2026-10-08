# RX_X2 finite-capture rearm retest — 2026-10-09

## Result

Repeated the previously failing native RX_X2 close/rearm profile with the
warning-only callback ring snapshot enabled: 100 cross-band transitions,
23.04 Msps, 32,768-sample stream buffers, 3,456,000 samples per lane
(150 ms), immediate capture close, and a 250 ms closed-epoch pause.

All 100 captures returned the requested sample count. The run reported zero
unrecovered transitions, first-read faults, retries, stream-overrun events,
and RX short reads. It recorded 200 expected `RX_DATA_WITHHELD` events during
closed-epoch periods. Event history was complete through epoch 100, both RX
lanes disabled cleanly, and device close completed. Transition latency was
P50 28.264 ms, P95 30.648 ms, P99 31.464 ms, maximum 31.648 ms.

The same run at 20 cycles also passed 20/20 with no ring-full. Full console
trace: `rxx2-finite-capture-warning-retest-100-20261009.log` (local). The
earlier failure at this geometry remains valid historical evidence; these
clean repeats do not establish the root cause or close the release gate.

## Disposition

The warning-only callback snapshot did not trigger during this retest, so the
race remains unobserved at its failure point. Continue with longer LTE/DSP
soaks and preserve the callback snapshot for any recurrence. Do not change
ring logic or claim stable release based only on clean repeats.
