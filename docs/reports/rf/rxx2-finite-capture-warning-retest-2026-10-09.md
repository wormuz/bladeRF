# RX_X2 finite-capture rearm retest — 2026-10-09

## Result

Repeated the previously failing native RX_X2 close/rearm profile with the
warning-only callback ring snapshot enabled: 1,000 cross-band transitions,
23.04 Msps, 32,768-sample stream buffers, 3,456,000 samples per lane
(150 ms), immediate capture close, and a 250 ms closed-epoch pause.

All 1,000 captures returned the requested sample count. The run reported zero
unrecovered transitions, first-read faults, retries, stream-overrun events,
and RX short reads. It recorded 2,000 expected `RX_DATA_WITHHELD` events during
closed-epoch periods. Event history was complete through transaction 1000,
both RX lanes disabled cleanly, and device close completed. Transition
latency was P50 27.856 ms, P95 32.606 ms, P99 35.933 ms, maximum 40.245 ms.

The earlier 20- and 100-cycle repetitions at 250 ms also passed without
ring-full. A closer processing-gap profile then passed 100/100 with a 750 ms
closed-epoch pause (P99 35.635 ms, maximum 40.255 ms), also with zero short
reads and overruns. Full 1,000-cycle console trace:
`rxx2-finite-capture-warning-retest-1000-20261009.log` (local). The earlier
failure at this geometry remains valid historical evidence; these clean
repeats do not establish the root cause or close the LTE/Python release gate.
The 750 ms console trace is `rxx2-finite-capture-warning-retest-750ms-100-20261009.log` (local).

## Disposition

The warning-only callback snapshot did not trigger during this retest, so the
race remains unobserved at its failure point. Continue with longer LTE/DSP
soaks and preserve the callback snapshot for any recurrence. Do not change
ring logic or claim stable release based only on clean repeats.
