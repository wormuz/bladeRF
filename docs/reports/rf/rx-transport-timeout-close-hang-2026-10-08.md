# RX USB timeout and shutdown hang — 2026-10-08

## Reproduction

Ran `run_rx_epoch_transition_qualification.sh 10 RX1 --cross-band` against
the attached xA4, then repeated with one transition under GDB. No services or
FPGA reload were involved. The local executable resolves
`host/build/output/libbladeRF.so.2`; the board enumerates at SuperSpeed and
reports firmware `2.6.1-git-fc02ffd8`, FPGA `16.1.0`.

The 10-transition run saw RX USB timeout at 0/32768 bytes after 10 complete
transfers, followed by runtime RX overrun events (`0x10000009`, USB async;
`0x10000011`, timestamp discontinuity). The stream then returned WOULD_BLOCK
for the remaining transitions. A one-transition GDB run saw a partial
8192/32768-byte timeout after 21 complete transfers; the event history again
reported both faults. The host did not expose a successful LTE-validity claim
for this failed qualification.

## Shutdown failure

After the failed series the harness disables RX. GDB captured this call chain:

```text
bladerf_enable_module(dev, RX1, false)   [caller owns dev->lock]
  _rfic_host_enable_module()
    sync_deinit()
      sync_worker_deinit()
        stop wait expires (3 s)
        pthread_cancel(worker)
        pthread_join(worker)             [does not return]
```

At the blocked join, the RX worker remained in
`libusb_handle_events_timeout_completed()` / `libusb_unlock_events()`. The
libusb event thread was polling, and the RX fault monitor was waiting for
`dev->lock`, which the caller held across `sync_deinit()`. A 45-second
external watchdog terminated the isolated repro; the unbounded join is the
confirmed host-side hang. This is independent of whether the underlying USB
timeout is caused by the device, endpoint, or host controller.

## What this proves and next step

Native event reporting correctly surfaced transport timeout and the resulting
timestamp discontinuity before returning invalid/withheld data. It does not
yet explain why the endpoint stopped completing full transfers. It does prove
that the sync-worker fallback cancellation is not a bounded shutdown: after a
real transport fault, close can wait forever and hold the device lock.

An experiment added a per-stream `libusb_interrupt_event_handler()` call when
the sync worker received STOP. Debug output confirmed it was called, but the
same 3-second worker-stop timeout and unbounded join remained. That ineffective
change was reverted. Next: use a breakpoint/trace on the libusb event-lock
owner and URB callback accounting to find why cancelled transfers do not drive
`num_avail` to `num_transfers`; then fix the lock/lifecycle path and add a
regression that injects an RX timeout and asserts bounded join, event order,
and subsequent stream recovery. Do not treat the FPGA image version string as
proof of image identity; the current probe did not load an image.

Raw runs: `/tmp/rx1_epoch_qual_10.log`, `/tmp/rxqual-failure.log`.
GDB/strace traces remain local in `/tmp` and were not committed.

## Follow-up: stream restart and remaining join hang

Inspection found that `lusb_stream_data.done_flag` was initialized only when
the backend stream was created. A terminal stream run sets it to 1; sync RX
can report that error, return to idle, and invoke the same backend stream
again. `libusb_handle_events_timeout_completed()` treats a nonzero completion
flag as already complete, so a stale value can prevent the next run from
reaping transfer callbacks. The backend now resets this per-run flag before
submitting transfers. This is a valid stream-restart lifecycle fix, but it is
not a complete fix for the close hang.

After the reset, another GDB run still blocked in `pthread_join()` after an RX
USB timeout. At the join breakpoint, `sync_worker` had state `STOPPED`, the
stream had state `STREAM_DONE` with `error_code=-6`, `done_flag=1`, and all 8
transfer slots were available (`num_avail=8`, `num_transfers=8`). The observed
thread stacks did not yet establish why the join target remained live. This
changes the next investigation: verify the exact `pthread_t` passed to join
against created thread IDs and trace the worker's return/cleanup path; do not
assume that pending USB callbacks are still the only cause.

The test-only injected `ERROR` run also failed before it could qualify bounded
shutdown: the test expected a withheld-event reason of `0x20`, while the
observed event carried `0x10000020`; it then reported `withheld=0`. That is a
separate event-mask/test expectation issue, and this run is not evidence for
or against the join fix. The checker now compares only the documented
withheld-reason bits. With this correction, RX1 and RX2 each pass injected
`ERROR`, `STALL`, `TIMEOUT`, `NO_DEVICE`, and `UNKNOWN` statuses with bounded
cleanup. The checker now allows a timestamp-discontinuity notification to
precede the injected error, while still requiring the expected fault reason
and overrun. The delayed injection (`TIMEOUT` after 20 completed transfers)
did not reach the terminal transfer-failure path because this harness ended
its capture before 20 full USB completions. A terminal `TIMEOUT` after four
full transfers does pass, exercising cancellation while other transfers are
in flight. These injected runs validate callback/error cleanup but do not
reproduce the physical xA4 fault or explain the separate hardware-only join
hang.

## Follow-up: worker startup timeout use-after-free

A later GDB run exposed a separate, concrete lifecycle race. When
`bladerf_sync_config()` timed out while waiting for the new worker to reach
`IDLE`, `sync_worker_init()` followed its generic error path, freed
`s->worker`, and returned without stopping or joining the thread created just
above. GDB caught that thread in `sync_worker_task()` with `s->worker == NULL`;
it faulted in `set_state()` while locking address `0x20`. This is a confirmed
use-after-free on the worker-startup timeout path. The path now requests stop
and joins the created worker before freeing its state; pre-thread failures
release only the stream and synchronization objects they initialized. Normal
worker deinit now also destroys its synchronization objects after join.

After this change, native sync worker-stop and epoch traversal tests passed.
A five-transition RX1 cross-band hardware run still failed qualification on
USB timeouts/overrun, but RX disable and device close both completed. This
shows cleanup completed in that run; it does not prove the startup-timeout
path under deterministic scheduling or close the physical USB fault. Log:
`/tmp/rx1-sync-worker-init-cleanup.log`.

The stop/join logic used by both normal deinit and startup-timeout cleanup is
now shared and directly tested with a thread that has not reported `IDLE`.
That race test passed 100/100 repetitions, and epoch traversal still passes.
This verifies the shared stop/signal/join sequence; it does not yet force the
full ten-second `sync_worker_init()` wait to expire in an integration test.

The corresponding RX2 five-transition run also terminated on USB
timeouts/overrun but completed both channel disables and `bladerf_close()`;
log: `/tmp/rx2-sync-worker-init-cleanup.log`. It additionally produced an
FPGA cause snapshot `rfic_status=0x80000004`: bit 31 marks a valid FPGA RX
fault-cause snapshot and bit 2 is `GPIF_TIMEOUT` (`fifo_writer.vhd`). The
watchdog latches this when RX sample-FIFO writes had occurred in the epoch
and then stopped for `2^22` RX clock cycles while the link remained active
and the epoch was not intentionally fenced. This is not an AD9361 status.
It narrows the failure to an RX datapath progress stall, but does not by
itself distinguish GPIF/FX3 backpressure (for example, a full RX FIFO) from
an upstream stop in sample writes. The field name `rfic_status` is reused by
the event schema with the documented `BLADERF_RF_FPGA_RX_FAULT_CAUSES_VALID`
marker; consumers must decode the marker before treating the value as RFIC
register state.

The board reported FPGA `16.1.0`, while source `fpga_version.h` carries ID
`0x7778` and major/minor `0.16`. That version readback is insufficient to
identify the exact bitstream build/hash, so the observed watchdog behavior
cannot yet be attributed to a particular Quartus image.

To distinguish the two remaining sources of a GPIF progress stall, RX
fault-cause bit 5 is now reserved as `FIFO_FULL_AT_STALL`. It latches with
`GPIF_TIMEOUT` when the watchdog expires while `fifo_full` is asserted; a
cause with bit 2 but not bit 5 means RX FIFO writes stopped while the FIFO was
not full. The NIOS cause PIO, libbladeRF decoder, and public event flag
definitions now carry this detail. GHDL passes the directed RX writer test
(including full-FIFO classification) and the armed, abort, and enable tests.
The `698dbb6e` HDL was synthesized and fitted in Quartus 25.1 for hosted xA4.
Fitter succeeded with no negative timing slack (worst reported setup +0.552
ns, hold +0.007 ns), and produced RBF SHA-256
`14dc3ea9eecdb3f02438eec71ac2bc55d11bbd8976d5540583a5c8491e075454`. The
release qgate rejected this `hosted` revision: its current CDC expectations
do not match that older project variant (12 handshake crossings vs expected
5, D101=674, D103=0, and 85 critical warnings). This is a gate failure, so
the image was not loaded. The `sweep` xA4 compile on `9ab05e1d` completed
fit and produced RBF SHA-256
`69c086bbc6c952e2b123b8202cece7740004ae5e03b46863245f9fbb3b6e5035`, but
qgate rejected it for three timing violations: RX AD9361 `pll_sclk` generated
clock setup slack −0.190 ns at 85°C and −0.221 ns at 0°C, plus hold slack
−0.035 ns at 0°C. All max-skew paths and paired CDC bundle checks passed.
The Design Assistant inventory is 740 D101 structures, including 409 held
handshake bits (the 16-bit increase is the new RX fault-cause status
handshake), 128 tamer, 129 ADI transfer, 10 ADI status, and 64 clock-monitor
bits. qgate/qcheck were updated to validate this exact inventory; qcheck is
clean and a direct qgate rerun now fails only on the three timing entries.
Seed 5 on the same RTL closed timing with worst setup +0.018 ns and hold
+0.012 ns, and passed qgate with all 740 D101 sources classified. Its RBF
SHA-256 is `4633839b5bc95eb4377ce9aa379e199083f4b58e5c9c100ad4208032130a215d`.

## Volatile xA4 diagnostic-image probe

After the seed-5 image passed qgate, I loaded its 2,632,660-byte RBF into
volatile FPGA RAM (no SPI flash write). The board's EEPROM size query remains
invalid, so the documented `BLADERF_FORCE_FPGA_A4=1` override was used for
this known xA4; the image length equals the driver's exact A4 length. The
first CLI process stayed blocked after USB re-enumeration and was interrupted;
a subsequent fresh `bladeRF-cli version` reported FPGA `0.16.1 (configured by
USB host)`, and the device reopened and initialized successfully.

On the loaded image, an RX2 cross-band probe reproduced two runtime overruns.
One event carried `rfic_status=0x80000004`: cause-snapshot-valid plus
`GPIF_TIMEOUT`, with `FIFO_FULL_AT_STALL` clear. This proves the sample FIFO's
`wfull` signal was low at the watchdog boundary; it does not yet prove samples
were absent, because META-FIFO backpressure and writer state were not captured.
The non-GDB run then hung at `FINAL_DEVICE_CLOSE_BEGIN` until a 120-second
test watchdog killed it. A second run under GDB reported two overruns and
four unrecovered transitions out of five, but completed `bladerf_close()`.
Thus host teardown remains intermittent.

To resolve the ambiguity in the FPGA cause snapshot, the next RTL change
captures five boundary fields alongside the sticky causes: `meta_fifo_full`,
enabled input `sample_valid`, writer `HOLDOFF`, `meta_written`, and
`fifo_enough` (space for a full DMA buffer). These occupy bits 6..10 of the
existing coherent NIOS cause word; RX data path behavior is unchanged. The
directed GHDL test now checks both a full-FIFO stall and a non-full stall
after valid samples stop. `qcheck`, the four fifo_writer GHDL benches, the
libbladeRF build, sync-worker stop test, and sync epoch traversal test pass.
Quartus seed 5 fit of this expanded snapshot on commit `20997366` completed
with RBF SHA-256
`47111735df594b632ba8b5ca16af98d8dfaefb4a1d17241381d133f2bb4d1213`, but
qgate rejected two generated `pll_sclk` setup entries (−0.123 ns at 85°C and
−0.220 ns at 0°C); hold slack remained positive. The five additional D101
bits exactly match the five context fields on the existing 32-bit CDC
handshake. qgate/qcheck now expect 745 total / 414 held-handshake bits and
classify the complete inventory; direct qgate rerun fails only on the two
timing entries. This expanded-context RBF has not been loaded.

Seed 1 fit of the same expanded-context RTL on commit `8e20ef85` also
completed synthesis/fitter/STA with all CDC and max-skew checks clean, but
qgate rejected two setup entries: `pll_sclk` slack −0.058 ns at 85°C and
−0.303 ns at 0°C (hold slack +0.213/+0.187 ns). Its RBF SHA-256 is
`dda3aa538ce2b7a2cd932c257281085089a948ff2a38a196f71bff697d4530f6`; it
was not loaded. Another fit seed or a timing-path correction is needed;
qgate criteria remain unchanged.

Seed 7 fit of the expanded-context RTL at commit `a8bf7981` completed
successfully through fitter and STA. CDC inventories (745 D101, 414 held
handshake bits), all max-skew paths, and the fitted bundled-data checks pass.
qgate rejects one setup entry: the generated AD9361 RX `pll_sclk` clock has
slack +0.040 ns at 85°C but −0.066 ns at 0°C (TNS −0.837 ns); hold slack is
positive at both corners. The sole critical warning is the corresponding
unmet timing requirement. RBF SHA-256 is
`74a8423d5dec7a5a87a8f84898678fca69ac55e11aad3a5e071135a42ed06a96`; it was
not loaded. The failure remains on the generated `pll_sclk` timing domain,
so the next action is to inspect the exact TimeQuest path and compare it with
the passing seed-5 fit before considering any RTL or constraint change.

Raw follow-up output: `/tmp/rx-terminal-injection-after-done-reset.log`.

## Sweep timing-path closure for the RX fault-context image

The cold-corner path report for the expanded fault-context image identified
the timing failure as an existing sweep-analysis path, not the new CDC
snapshot: `dwell_threshold[14]` through the trigger threshold comparator to
`dwell_summary.trig_time[*]` in `pll_sclk`. Seed 5 had five logic levels and
−0.220 ns slack at 0°C; seed 7 had four levels and −0.066 ns. The dominant
data path was 8.032 ns in seed 5 and 7.879 ns in seed 7, with approximately
−0.117 ns clock skew in both.

Commit `0391ce11` registers the per-window threshold comparison before
trigger-history consumption. The added cycle is compensated in the captured
timestamp. GHDL `dwell_summary_equiv_tb` compared 10 records field-by-field
with the frozen reference, `dwell_summary_tb` passed all seven cases, the
RX epoch-fence bench passed, and `qcheck` was clean.

The matching xA4 seed-7 fit completed and passed the full release `qgate`:
all 15 handshake pairs, 745 classified D101 sources (including 414 held
handshake bits), fitted bundled-data checks, and all max-skew paths passed.
Worst `pll_sclk` setup slack is +0.089 ns at 85°C and +0.137 ns at 0°C;
worst hold slack is +0.189 ns and +0.183 ns respectively. RBF SHA-256 is
`ab7aefef2fa72fde0cd7cd2ba67853a79aaf86b7f0f6dda4879657dea050d5d0`.
At fit completion the image had not yet been loaded; the volatile hardware
qualification result follows below. Static qgate success alone does not
establish runtime RX validity or recovery.

## Volatile hardware qualification on the timing-closed image

The exact job-74 RBF was loaded to volatile FPGA RAM on xA4 serial
`f695006ba84a40daa7b777c6a6eba78`; no SPI flash write was issued. The first
load attempt used the stale `/usr/local/bin/bladeRF-cli` and was rejected
because it linked `/usr/local/lib/libbladeRF.so.2`, which did not apply the
A4 override. Repeating with the repository's `host/build/output/bladeRF-cli`
and `LD_LIBRARY_PATH=host/build/output` accepted the known A4 override and
the exact 2,632,660-byte RBF. The loader process remained blocked across USB
re-enumeration and was ended by a 45-second watchdog; a fresh local CLI
reopened the device. The subsequent fault-context fields below are also
evidence that the new image is active.

RX2 cross-band qualification under GDB reported two stream overruns,
`transitions=5`, `unrecovered=4`, `first_read_faults=2`, `recovered=1`, and
`data_withheld_events=5`. The second overrun carried
`rfic_status=0x80000184`: valid cause snapshot, GPIF timeout, enabled input
sample valid, and FIFO writer in HOLDOFF. `FIFO_FULL_AT_STALL`, META FIFO
full, `meta_written`, and `fifo_enough` were all clear. Thus samples were
present while the writer was held off, and the snapshot indicates insufficient
room for a complete DMA buffer. It does not yet identify the exact FIFO
occupancies or prove why GPIF/USB stopped draining. The non-GDB run hung in
`bladerf_close()` through its 120-second watchdog; the GDB run closed
successfully, confirming the teardown hang remains intermittent.

RX1 cross-band qualification also failed: one USB timeout after ten full
transfers, one runtime overrun, four unrecovered transitions of five, and
five data-withheld events. It closed successfully under GDB. Paired `BOTH`
qualification failed all five transitions, with one stream overrun and
`data_withheld_events=2`; it also closed successfully under GDB. These are
transport/event failures, not LTE or RF-signal quality failures: the test
never received valid epochs for application IQ. No RX1, RX2, or paired
RX_X2 configuration is release-qualified.

Next diagnostic step: latch the sample- and META-FIFO occupancy values at
the first GPIF watchdog event into the unused upper bits of the existing
32-bit coherent fault snapshot. That will distinguish downstream FIFO
backpressure from a metadata handoff stall without adding another CDC. Then
inspect the FX3/GPIF progress side against the captured occupancies.

Commit `40974899` implements that occupancy snapshot. Cause bits 11..19 carry
sample FIFO occupancy in 16-entry units; bits 20..30 carry exact META FIFO
occupancy, leaving bit 31 for the host-side snapshot-valid marker. The
existing single 32-bit RX-clock-to-system-clock handshake remains unchanged.
`fifo_writer_epoch_fence_tb` now asserts the captured occupancies; all four
fifo-writer GHDL benches, `qcheck`, the libbladeRF shared-library build,
sync-worker stop test, and sync epoch traversal test pass. A fresh xA4 fit
and volatile hardware probe are still required before this diagnostic is
usable for root-cause closure.
