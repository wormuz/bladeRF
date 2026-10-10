# USB backend stream initialization cleanup audit — 2026-10-10

## Findings

The async stream layer tears down the backend stream object after a backend
initializer returns an error. Both supported USB backends also release their
own partial allocations before returning that error and clear
`stream->backend_data`. Their deinit functions did not accept this already
cleaned state: libusb dereferenced a null backend pointer, while CyAPI asserted
that the pointer was non-null. An allocation failure during normal libusb
transfer creation also leaked previously allocated transfers because its
cleanup loop skipped index zero.

CyAPI recorded the requested transfer count only after all event handles had
been created, so a partial event-creation failure skipped closing handles
already created.

The same backend review found that both USB implementations accepted a
successful but short TX completion. They then called the producer as though
the stream remained healthy, without raising the TX transport fault event.
Very large buffer sizes could also exceed the signed transfer-length types
used by libusb/CyAPI even when `size_t` arithmetic itself did not overflow.

## Change

libusb now allocates a zeroed transfer-pointer array, frees every allocated
transfer on failure, and treats a null backend state as an already-cleaned
stream during deinit. CyAPI records its transfer count before creating event
handles, closes each non-null handle during partial cleanup, and makes deinit
safe after backend initialization already cleaned its state.

Both USB backends now fail a short TX completion closed with `BLADERF_ERR_IO`;
the common stream return path publishes the TX transport failure. Async and
sync setup reject transfer lengths that exceed the backend's signed length
limit before submission.

These are shared backend lifecycle paths for RX and TX.

## Verification status

`git diff --check` passes. No test, build, simulation, synthesis, or device
operation was run while the full source defect sweep remains active.
