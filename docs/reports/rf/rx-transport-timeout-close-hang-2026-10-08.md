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

Raw follow-up output: `/tmp/rx-terminal-injection-after-done-reset.log`.
