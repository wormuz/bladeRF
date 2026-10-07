# RX fault-cause CDC and RX1/RX2 qualification — 2026-10-07

## Changes

- Added `U_rx_fault_causes_handshake` to the bladeRF Micro bundled-data SDC
  pairs. The FPGA now reports 14 constrained handshake crossings, including
  the fault-cause snapshot bus.
- Fixed FPGA reload initialization: `bladerf_load_fpga()` holds the RX
  reconfiguration reservation across image replacement and RFIC reinit. The
  internal PLL setup now reuses that reservation instead of recursively
  entering the public invalidation path and returning `BLADERF_ERR_WOULD_BLOCK`.
- Added setup-path diagnostic script `hdl/quartus/report_setup_diag.tcl`.

## Build and timing

Full Quartus flow for xA4 completed successfully in the isolated
`adr0207-rx-cause-full-xa4` project on 2026-10-07. Synthesis, fitter, STA, and
assembler reported zero errors. STA reported 14 handshake crossings
constrained, worst slow-corner setup slack +0.569 ns, and worst hold slack
+0.199 ns. The generated RBF is 2,632,660 bytes, SHA-256
`a6d9adcb2fea943cdbef42b88a40f247b6790a7ccea4b2d7e50f95e36823a1ac`.

## Hardware qualification

The generated RBF was loaded into the xA4 without writing flash. The board's
flash FPGA-size query returns `BLADERF_ERR_INVAL`, so loading used the known
xA4 override (`BLADERF_FORCE_FPGA_A4=1`) and local-image size-check override
(`BLADERF_SKIP_FPGA_SIZE_CHECK=1`). The FPGA reload and subsequent RFIC
initialization completed successfully after the reservation fix.

`host/misc/run_rx_transition_validity_live.sh` passed in all three modes:

- `RX1`: single-channel transition and IQ validity fencing passed.
- `RX2`: second-channel transition and IQ validity fencing passed.
- `BOTH`: RX1 and RX2 enabled together in RX_X2; transition and paired-stream
  validity fencing passed.

Each run also confirmed that legacy frequency, bandwidth, sample-rate, TX FIR,
configuration GPIO, and feature changes revoke the previous RX data-valid
certificate until a successful event-driven transition creates a fresh epoch.

## Remaining qualification

This validates channel selection, paired streaming, FPGA reload, and validity
notification on one xA4. A new `host/misc/run_rx_epoch_transition_qualification.sh`
qualification now runs event-order, stale-epoch filtering, completion, and
runtime-event checks across RX1, RX2, and RX_X2. It drains the global event
history by sequence, so transaction ID 0 overrun/withheld notifications are
included and history loss fails the run.

On 2026-10-07, 3,334 transitions per mode completed with no unrecovered IQ
read failures:

| Mode | P50 / P95 / P99 transition latency | Unrecovered | Runtime overrun events | Withheld notifications |
|---|---:|---:|---:|---:|
| RX1 | 6.909 / 7.201 / 7.427 ms | 0 | 0 | 3,334 |
| RX2 | 6.909 / 7.207 / 7.671 ms | 0 | 0 | 3,334 |
| RX_X2 | 6.913 / 7.205 / 7.431 ms | 0 | 7 | 3,337 |

All three modes returned `WOULD_BLOCK` on the first sync read after every
transition, then recovered on a later read (3,334/3,334 in each mode). This
is the expected fail-closed behavior while the next valid host packet has not
arrived. Each transition also emitted its runtime data-withheld notification.
The seven RX_X2 overrun events are real qualification failures even though
the sample reads recovered; their flags were `BLADERF_RF_STREAM_STATUS_OVERRUN`
without `BLADERF_RF_STREAM_STATUS_FPGA_RX_LOSS`, so they identify a host
stream/queue discontinuity rather than an FPGA sample-path loss. The current
harness now exits nonzero if any such event occurs. A separate 1,000-transition
RX_X2 rerun saw two overrun events on the first transition (epoch 1); a later
100-transition rerun saw two on its first transition (epoch 101), and another
100-transition rerun saw none. This is intermittent and needs follow-up
before claiming zero-loss RX_X2 qualification. The public event now carries
source bits for sync RX queue loss, async USB transport errors, timestamp
discontinuity, runtime RF-state faults, and FPGA loss-counter events. A
follow-up 1,000-transition RX_X2 run reproduced 11 failures, all classified
as `sync_rx_queue` plus `sync_rx_ring_full` (`flags=0x45`); none indicated
FPGA sample-path loss. A later 3,334-transition run also found 11 ring-full
events. The sync worker now preserves this subcause in the event, narrowing
the remaining work to RX_X2 queue occupancy and consumer throughput rather
than RFIC or FPGA loss detection. After adding source and subtype reporting,
another 1,000-transition RX_X2 run had zero overrun events; 1,000-transition
RX1 and RX2 runs also had zero. The RX_X2 ring-full condition is intermittent,
so the earlier 3,334-transition qualification remains failed and must be
repeated at scale after the queue behavior is understood.

The aggregate run covers 10,002 transitions, but it does not replace physical
fault injection for every FPGA fault-cause bit or a larger repeated RX_X2
zero-overrun qualification.

## Concurrent RX_X2 consumer follow-up

The host sync read now publishes a cancellation generation before waiting for
the sync mutex. A read already blocked when a transition starts returns
`BLADERF_ERR_WOULD_BLOCK` promptly, and its final delivery check is linearized
against invalidation so an old buffer cannot escape after the transition
revokes its epoch. The RX transition preflight checks the configured metadata
format under the device lock, avoiding a blocking sync-mutex acquisition ahead
of that cancellation path. NIOS control request/response transfers are also
serialized across legacy and current packet formats because the shared USB IN
endpoint carries replies for both.

Live xA4 RX_X2 test with a concurrent sync consumer:

- 1 transition: success; 245 post-transition blocks; no invalid blocks,
  timestamp regressions, or overrun events.
- 100 transitions issued back-to-back: all 100 control transitions succeeded,
  but the run failed qualification with sync ring-full/overrun events and only
  151 valid blocks. This deliberately zero-dwell stress run confirms fault
  notification works; it does not qualify usable sweep throughput. It also
  shows that `EPOCH_VALID` can precede host consumption of any data from that
  epoch. A sweep caller must leave a data-collection interval between retunes,
  and a future API completion condition should distinguish FPGA epoch opening
  from first valid host-delivered data.

The separate RX1, RX2, and paired RX_X2 basic validity tests remain passed.
Full-rate concurrent RX_X2 qualification is still open until a sweep-style
test with a defined per-channel collection interval has zero queue overruns.

Follow-up isolated the remaining burst issue and closed the stale-buffer part
of it. `sync_rx_epoch_invalidate()` now releases completed and partially
consumed buffers from the revoked epoch, resets an active META parser at that
boundary, and leaves the dropped-slot markers for normal ring traversal. The
transition does not mark those intentionally revoked samples as transport
loss, and the reset does not certify any later samples; only the existing
FPGA epoch/timestamp path can do that. The parser reset is limited to active
buffer states so an idle/stopped worker still follows its normal startup path.

The concurrent RX_X2 harness now supports an explicit dwell interval, sample
rate, and stream buffer size. On this xA4, the following results were
reproduced:

| Transitions | Sample rate per RX | Stream buffer | Dwell | Valid blocks | Invalid blocks | Overrun events |
|---:|---:|---:|---:|---:|---:|---:|
| 100 | 4 MS/s | 8,192 samples | 10 ms | 359 | 73 | 88 |
| 100 | 1 MS/s | 8,192 samples | 10 ms | 120 | 0 | 0 |
| 100 | 4 MS/s | 65,536 samples | 10 ms | 458 | 0 | 0 |
| 1,000 | 4 MS/s | 65,536 samples | 10 ms | 2,074 | 0 | 0 |

The thousand-transition run also had zero event-history gaps, zero sync
overruns, and zero timestamp regressions. After the first-transition filter
activation fix, the RX1, RX2, and paired RX_X2 validity runners all passed
again. A 100-transition zero-dwell run with 8,192-sample buffers also passed
after stale-buffer revocation was added.
The comparison indicates that the default small transfer buffer lacks burst
headroom for sustained 4 MS/s RX_X2 consumption; larger stream buffers remove
the observed overruns in this test without adding validity sleeps or sample
discard. This result qualifies the tested xA4 host configuration, while the
earlier 3,334-transition run remains a valid failure for its then-current
configuration and must not be rewritten as a pass.

## Re-arm sync fault notifications after recovery

The sync RX valid-data callback already emitted `RX_FIRST_VALID_HOST_DATA` or
`RX_DATA_RESUMED`, but only the asynchronous META admission path cleared the
withheld-event deduplication latches after valid IQ returned. As a result, a
second independent parser/epoch fault in the same sync epoch could be hidden
behind the first `RX_DATA_WITHHELD` event. The shared notification-rearm
helper now runs after a certified sync packet and on asynchronous admission;
it clears only notification coalescing state and never changes epoch
certification. A later fault therefore starts a new reported withheld
interval, while repeated reports during one uninterrupted interval remain
coalesced.

The libbladeRF build and `libbladeRF_test_sync_epoch_traversal` pass with a
regression that rearms the latch and confirms a second fault appends another
event. This is a host-side state/notification test; no new physical fault
injection or hardware qualification is claimed for this follow-up.

## Keep transition completion separate from host-data events

`RX_FIRST_VALID_HOST_DATA` and `RX_DATA_RESUMED` carry the transaction ID of
the epoch they describe. A fast RX consumer can publish one of these between
the transition's `RX_EPOCH_VALID` event and the waiter's final history read.
The result selector previously returned the latest event with that
transaction ID, so a successful wait could nondeterministically return a
host-data lifecycle event instead of the transition completion event. The
selector now skips post-transition lifecycle events and returns only a
transition-terminal event. If event-ring pressure removed every terminal
event, the wait fails closed instead of returning `CONFIG_ACCEPTED` as a
success. `bladerf_rx_transition_get_events()` also requires a retained
transition-terminal event to claim complete history; a later host-data event
cannot conceal eviction of the completion event. The history still retains
and exposes all events when present.

The policy regression exercises `CONFIG_ACCEPTED → RX_EPOCH_VALID →
RX_FIRST_VALID_HOST_DATA → RX_DATA_RESUMED` and requires the waiter result to
remain `RX_EPOCH_VALID`; a second case removes that terminal event and requires
selection to fail. The terminal classifier also rejects both host-data event
types as transition completion. `run_rx_transition_policy_test.sh`, the rebuilt
libbladeRF target, and `libbladeRF_test_sync_epoch_traversal` passed. Hardware race
qualification remains pending. The board enumerates, but the live handoff
runner currently fails during `bladerf_open()` while reading FPGA version
(`BLADERF_ERR_TIMEOUT`), so no live race result is claimed.

## Preserve first-host-data identity across event-ring pressure

The first-host-data publisher previously searched the bounded event ring for
the `RX_EPOCH_VALID` record to recover transaction identity. A saturated ring
could evict that record before host IQ arrived, suppressing the corresponding
`RX_FIRST_VALID_HOST_DATA` event even though the epoch certificate and packet
were valid. The epoch-valid event is now also retained as a lock-protected
snapshot with a per-epoch host-data-reported latch. First-data and resume
events are built from that snapshot, independent of ring retention.

The sync/epoch regression fills the ring with unrelated overrun events so the
epoch record is absent, then verifies the host-data and resume notifications
retain transaction 77 and epoch 5. The rebuilt target and
`libbladeRF_test_sync_epoch_traversal` pass. Hardware qualification remains
pending because the board's open path currently times out at FPGA-version
readback.
