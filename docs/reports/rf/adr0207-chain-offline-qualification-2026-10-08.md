# ADR-0207 chain qualification, 2026-10-08

## Result

The candidate FPGA image was loaded into xA4 volatile FPGA RAM, the AD9361
initialized, and the event-driven RX1/RX2/RX_X2 chain completed 10,000
production-geometry transitions across local and cross-band classes with zero
unrecovered reads and zero stream-overrun events. Every transition's first
live-edge read returned WOULD_BLOCK and later recovered only after META epoch
and timestamp validation; no IQ was accepted before that validation.

Four live RX_X2 LTE runs at 1.835 GHz confirmed PSS, PCI 85, 100 RB, and four
antenna ports. Each run also reported one later sync-ring-full event while
Python decoded the already captured block and no longer consumed the continuing
RX stream. The captured block passed count, epoch, timestamp, and overrun
checks before decoding. This later event is correctly surfaced by the stack;
the scanner's post-capture stream-consumer policy remains a release gate.

The event-driven hardware path is now strongly qualified at the tested
production stream geometries. A stable software release still requires
resolving calibration packaging and the scanner's post-capture
consumer behavior; the default system `/usr/local` libbladeRF also remains
older than this fork.

## Image recovery and identity

The xA4 enumerated over USB, but normal open timed out reading legacy NIOS FPGA
version byte 0 because NIOS did not reply. FX3/USB remained responsive. The
reusable volatile recovery path opens with FPGA probing skipped, removes that
override before configuration-status polling, and loads the candidate RBF.
It is in `host/misc/run_rx_fpga_recovery_load_live.sh` and
`host/misc/rx_fpga_recovery_load_live.c`; the script pins the candidate SHA-256
so an unintended image is rejected. It loaded and read back FPGA v0.16.1.
No FPGA flash write was performed.

Read-only flash backups rule out a missing FPGA image: the A4 autoload page
contains `FMT=LZMA`, `ULEN=2632660`, and `CLEN=683669`. The persistent decoder
`host/misc/verify_fpga_autoload_backup.py` decompressed that stream to a valid
raw RBF (SHA-256
`3ed6ae6c1502436c3f4b14e90318b15f33c116ebbde4059dcddf630447adeb8d`). It
differs from the current source candidate, but loads over USB and reports FPGA
v0.16.1 with working NIOS/AD9361. The source candidate was then reloaded and
verified again. The calibration region has no readable `B` (FPGA size) or
`DAC` (VCTCXO trim) key/value records; its data is erased (`0xff`). This
explains `FPGA size: Unknown` and fallback DAC trim `0x1ffc`. It does not
explain why the FX3's valid autoload stream did not leave NIOS responsive in
the original session. The FPGA reload used for recovery destroyed the original
volatile failure state; the initial failure was not captured before that
reload, so its exact trigger cannot be inferred from the later successful
opens. Do not reload the FPGA before collecting a first-request trace on a
future affected cold boot.

The persistent read-only probe is `host/misc/run_rx_nios_boot_probe.sh`. It
selects FX3 RF-link USB altsetting 1, sends exactly one 16-byte legacy NIOS
read for FPGA version byte 0, records OUT/IN status and elapsed time, then
returns to idle altsetting 0. It never sends FPGA configuration commands,
retries, or writes flash. On the already recovered live candidate the first
request completed in 0.128 ms and returned version byte 0; this validates the
probe and current NIOS responsiveness, not the lost boot failure.

Candidate: `hdl/quartus/work/bladerf-micro-A4-sweep/output_files/sweep.rbf`

SHA-256: `f53cd1c1fbdc8176c40397d1881a70b5081e8d489854928fb7732dc20ab4fa12`

## Production-geometry hardware results

The persistent runner is `host/misc/run_rx_epoch_transition_qualification.sh`.
`epoch_settle_samples=0`; no sleeps or sample-discard arithmetic are used. It
validates transaction/event order, RX channel provenance, META epoch/timestamp,
first-valid host data, event-history behavior, and stream-overrun count.

| Mode / class | Transitions | Sync geometry (buffers / samples / transfers) | Overruns | Unrecovered | P50 / P95 / P99 |
|---|---:|---|---:|---:|---:|
| RX1 local ±100 kHz | 1,000 | 512 / 131,072 / 24 | 0 | 0 | 9.520 / 12.301 / 13.172 ms |
| RX2 local ±100 kHz | 1,000 | 512 / 131,072 / 24 | 0 | 0 | 9.756 / 12.477 / 13.355 ms |
| RX_X2 local ±100 kHz | 3,334 | 64 / 32,768 / 32 | 0 | 0 | 6.962 / 7.245 / 7.561 ms |
| RX_X2 local ±100 kHz | 1,666 | 64 / 32,768 / 32 | 0 | 0 | 6.959 / 7.280 / 9.721 ms |
| RX1 cross-band 947.5 MHz ↔ 1.835 GHz | 1,000 | 512 / 131,072 / 24 | 0 | 0 | 21.428 / 24.947 / 29.814 ms |
| RX2 cross-band 947.5 MHz ↔ 1.835 GHz | 1,000 | 512 / 131,072 / 24 | 0 | 0 | 21.373 / 25.576 / 30.116 ms |
| RX_X2 cross-band 947.5 MHz ↔ 1.835 GHz | 1,000 | 64 / 32,768 / 32 | 0 | 0 | 20.003 / 20.577 / 22.078 ms |

These runs total exactly 10,000 transitions. All 10,000 had the expected
fail-closed first-read WOULD_BLOCK and a later validated recovery; all had zero
retry overrun metadata, zero `RX_STREAM_OVERRUN` events, and one withheld-data
notification per transition.

### Small-buffer stress and queue cause

The same cross-band RX_X2 stress with the deliberately small 8,192-sample,
16-buffer, 8-transfer geometry overran 996/1,000 times. Events carried
`SYNC_RX_QUEUE | SYNC_RX_RING_FULL` (`flags=0x45` plus RX1 transition-channel
provenance), not `FPGA_RX_LOSS`. Raising the buffer to 65,536 with 16 buffers
and 8 transfers gave 0/1,000 overruns; the actual RX_X2 application default
32,768/64/32 also gave 0/1,000. Single-lane cross-band tests also pass at the
actual 131,072/512/24 defaults. This isolates the stress failure to inadequate
host sync queue headroom at the small geometry, not an RF or FPGA sample-path
loss.

The persistent runner accepts
`BLADERF_QUAL_STREAM_BUFFER_SAMPLES=8192|32768|65536|131072`; 32,768 selects
the paired default 64/32 geometry, 131,072 selects the single-channel default
512/24 geometry, and the smaller values retain a controlled stress profile.
The runner emits terminal breadcrumbs for event history, RX disable, and device
close so large-pool runs remain diagnosable.

### LTE return and post-capture event

Four live one-frequency RX_X2 LTE runs at 1.835 GHz decoded PCI=85, 100 RB,
four antenna ports. They used the existing `hs_65536_pool512_t32` stream
profile and the wrapper's bounded RX_NOW retry. Each run emitted one later
`SYNC_RX_QUEUE | SYNC_RX_RING_FULL` event while Python decoded the already
captured clip and the stream remained active without a consumer. The wrapper
reported the event; the capture had already passed full-count, epoch,
timestamp, and META overrun checks. This does not indicate stale IQ in the
accepted clip, but leaves the running scanner's queue idle-time policy to fix
before the detector chain is resumed.

## Software and FPGA checks

- Current-source Quartus A4 seed-3 fit and qgate pass; fitted RBF hash above.
- `hdl/quartus/qcheck` is clean.
- `cmake --build host/build -j2` passes.
- Native RX epoch metadata, status identity, transition-policy, and sync
  traversal tests pass.
- GHDL `rx_epoch_controller_tb`, `rx_epoch_gate_tb`, and
  `fifo_writer_epoch_fence_tb` pass.
- Python wrapper 29 tests and scanner transition/epoch integration 31 tests
  passed against the fork library.
- Scanner dual-RX epoch + LTE dispatch focused tests: 31 passed after adding
  bounded RX_NOW/WOULD_BLOCK retry.
- Python cp314 wheel builds and imports with the current library selected.
  The system `/usr/local` libbladeRF remains older and is not a compatible
  release pairing.

The compatibility table now has an exact FPGA v0.16.1 / firmware v2.6.1 row.
`cmake --build host/build -j2` passed, and the rebuilt CLI opened the connected
xA4 and reported FPGA v0.16.1 without the previous "newer than entries"
warning. It still reports missing FPGA-size and VCTCXO trim keys because the
calibration flash region is erased.

## Priority plan to stable release

The earlier 10,000-transition campaign qualifies retune at its tested stream
geometries; it does not qualify the newly added finite-capture close/rearm
lifecycle. The top release blocker is native sync ordering after close.

1. **P0 — Finish native close→rearm qualification.** Live RX_X2 at 64 buffers
   / 32 transfers reproduced ring-full after close/rearm (2/3 and 9/10 cycles
   at 250 ms pause). Debug trace localized the queue overrun: the producer
   reached a FULL destination while 32 transfers remained in flight, before
   the sync consumer retired enough buffers. A native bladeRF 2.x META RX ring
   floor of 3× active transfers now gives enough headroom without wrapper
   tuning. With the caller still requesting 64/32, the library allocated the
   safe ring and 20 cross-band RX_X2 close/rearm cycles passed with zero
   overruns and clean teardown; explicit 96/32 independently passed 20/20.
   The caller's 64/32 request then passed 50 cross-band RX_X2 cycles with
   250 ms pauses; RX1 and RX2 each passed 20 cycles with zero overrun. An
   uninstrumented RX1 20-cycle run hung twice during device close, while one
   strace run completed, so teardown remains intermittently unqualified.
   Isolate the intermittent RX1 teardown hang and investigate the earlier
   timestamp-discontinuity trace. Source audit also found no dedicated reset of
   the FPGA sample/meta FIFOs on an RF epoch boundary. Keep this as an open
   hypothesis to test, not an asserted root cause.
2. **P1 — Complete extended close/rearm qualification.** Run at least 1,000
   finite capture → close → decode-length pause → rearm cycles per RX1, RX2,
   and RX_X2. Require no old-epoch IQ, ordered event history, continuous
   timestamps within each epoch, no unexplained sync/FPGA overrun, and clean
   teardown after every run.
3. **P2 — Re-run RF/LTE hardware qualification.** Exercise local and cross-band
   transitions and known-cell return points, including PCI 85, after the P0
   fix, using production stream geometries. Publish transition and first-valid
   latency distributions and capture/decoder results.
4. **P3 — Produce and smoke-test one matched release set.** Package firmware,
   FPGA image, libbladeRF and Python wrapper together; install that set and run
   smoke and regression tests against the installed artifacts. Local sibling
   checkout auto-selection fixes development ABI drift but is not a release
   artifact.
5. **P4 — Close board/image maintenance risks.** Resolve the recorded FPGA
   version discrepancy (`v0.16.1` vs `v16.1.0`), decide/document the erased
   FPGA-size and VCTCXO calibration-key handling before any flash write, and
   preserve a first-request trace if NIOS no-response recurs. The original
   NIOS failure cause remains unknown.

Generic detector fan-out stays sequenced after P0–P3. Correctness remains
hardware-event-driven: timeout reports failure and never certifies IQ; no
caller sleep or discard arithmetic is part of the valid-data path.
