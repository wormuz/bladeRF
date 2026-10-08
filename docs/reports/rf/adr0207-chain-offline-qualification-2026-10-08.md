# ADR-0207 chain qualification, 2026-10-08

## Result

The candidate FPGA image was loaded into xA4 volatile FPGA RAM, the AD9361
initialized, and the event-driven RX1/RX2/RX_X2 path passed 10,002 local-hop
transitions. Cross-band tests found a real host queue limit: paired RX_X2 with
8,192-sample sync buffers overran on 996/1,000 transitions, while the same
1,000-transition run with 65,536-sample buffers had zero overruns. Single-lane
cross-band runs passed at 1,000 transitions each with the small buffers.

The chain is substantially hardware-qualified but is not yet a stable release.
The small-buffer RX_X2 cross-band result is a configuration failure, not a
pass. LTE RX_X2 capture/decoding at 1.835 GHz recovered PSS, PCI 85, 100 RB,
and four antenna ports. Each live LTE run reported one later sync ring-full
overrun while CPU-side LTE decoding continued and the RX consumer was idle;
the already captured block passed epoch/timestamp validation. RX_X2 wrapper
retry and larger-buffer sweep capacity still need integration/qualification.

## Image recovery and identity

The xA4 enumerated over USB, but normal open timed out reading legacy NIOS FPGA
version byte 0 because NIOS did not reply. FX3/USB remained responsive. The
reusable volatile recovery path opens with FPGA probing skipped, removes that
override before configuration-status polling, and loads the candidate RBF.
It is in `host/misc/run_rx_fpga_recovery_load_live.sh` and
`host/misc/rx_fpga_recovery_load_live.c`; the script pins the candidate SHA-256
so an unintended image is rejected. It loaded and read back FPGA v0.16.1.
No FPGA flash write was performed.

Candidate: `hdl/quartus/work/bladerf-micro-A4-sweep/output_files/sweep.rbf`

SHA-256: `f53cd1c1fbdc8176c40397d1881a70b5081e8d489854928fb7732dc20ab4fa12`

## Hardware results

The persistent runner is `host/misc/run_rx_epoch_transition_qualification.sh`.
It validates event ordering, RX channel provenance, META epoch/timestamp,
first-valid data, event-history behavior, and the absence of stream-overrun
events. `epoch_settle_samples=0`; no sleeps or sample-discard arithmetic are
used. The first live-edge read returned WOULD_BLOCK for each transition, then a
later read recovered successfully; no IQ was accepted before epoch/timestamp
validation.

| Mode / transition class | Count | Buffer | Overruns | Unrecovered | P50 / P95 / P99 transition |
|---|---:|---:|---:|---:|---:|
| RX1 local ±100 kHz | 3,334 | 8,192 | 0 | 0 | 6.907 / 7.202 / 7.557 ms |
| RX2 local ±100 kHz | 3,334 | 8,192 | 0 | 0 | 6.901 / 7.170 / 7.494 ms |
| RX_X2 local ±100 kHz | 3,334 | 8,192 | 0 | 0 | 6.911 / 7.196 / 7.441 ms |
| RX1 cross-band 947.5 MHz ↔ 1.835 GHz | 1,000 | 8,192 | 0 | 0 | 20.181 ms P50 |
| RX2 cross-band 947.5 MHz ↔ 1.835 GHz | 1,000 | 8,192 | 0 | 0 | 20.173 ms P50 |
| RX_X2 cross-band 947.5 MHz ↔ 1.835 GHz | 1,000 | 8,192 | 996 | 0 | 20.295 ms P50 |
| RX_X2 cross-band 947.5 MHz ↔ 1.835 GHz | 1,000 | 65,536 | 0 | 0 | 20.076 / 20.790 / 21.916 ms |

The RX_X2 small-buffer events carried `SYNC_RX_QUEUE | SYNC_RX_RING_FULL`
(`flags=0x45`, plus RX1 transition-channel provenance). They did not carry
`FPGA_RX_LOSS`; FPGA loss counters were zero. Increasing only the host sync
buffer from 8,192 to 65,536 samples removed all 996 events without adding a
settling sleep or caller-side discard. This confirms queue headroom as the
cause for that stress pattern. The qualification runner now accepts
`BLADERF_QUAL_STREAM_BUFFER_SAMPLES=65536` for this explicit comparison; its
8,192 default remains the small-buffer stress case.

Four one-frequency RX_X2 LTE runs at 1.835 GHz confirmed PSS and decoded
PCI=85, 100 RB, four antenna ports. The runner used the existing
`hs_65536_pool512_t32` stream profile. Each run emitted one later
`SYNC_RX_QUEUE | SYNC_RX_RING_FULL` event while Python was decoding and no
consumer was draining the continuing RX stream. The event was surfaced through
the wrapper; the captured LTE block had already passed full-count, epoch,
timestamp, and overrun checks. This is a post-capture stream-consumer issue,
not evidence that the certified capture contained stale samples. The RX_X2
wrapper now retries a bounded number of nonblocking RX_NOW reads when the
current epoch has not delivered a block yet; it never accepts a block without
metadata validation. Focused wrapper and LTE dispatch tests pass.

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
  bounded RX_NOW/WouldBlock retry.
- Python cp314 wheel builds and imports with the current library selected.
  The system `/usr/local` libbladeRF remains older and is not a compatible
  release pairing.

## Remaining release gates

1. Decide and qualify production RX_X2 stream geometry for cross-band retunes;
   the 8,192-sample profile is demonstrably insufficient for the tested
   no-dwell cross-band stress, while 65,536 passes.
2. Qualify the scanner's post-capture RX consumer policy so CPU decode time
   cannot leave an unconsumed stream running into a reported ring-full event.
3. Update libbladeRF's FPGA compatibility table for v0.16.1 and investigate
   this board's unavailable FPGA-size and factory VCTCXO flash reads. The host
   fell back to DAC trim `0x1ffc`; RF operation initialized, but release should
   record the board calibration state explicitly.
4. Re-run the exact production RX1, RX2, RX_X2 stream profiles at scale, then
   produce/install a matching libbladeRF + Python wrapper release artifact.
5. Keep detector fan-out work sequenced after these chain-level release gates.
