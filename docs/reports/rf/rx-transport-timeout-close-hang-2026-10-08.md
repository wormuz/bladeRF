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

Next: instrument/wake the libusb event loop during stream shutdown, remove any
device-lock hold across blocking sync teardown, and add a regression that
injects an RX transfer timeout and asserts bounded worker join, event order,
and subsequent stream recovery. Do not treat the FPGA image version string as
proof of image identity; the current probe did not load an image.

Raw runs: `/tmp/rx1_epoch_qual_10.log`, `/tmp/rxqual-failure.log`.
GDB/strace traces remain local in `/tmp` and were not committed.
