# Async and sync stream lifecycle audit — 2026-10-10

## Findings

The RX/TX stream initialization and teardown paths had several shared defects:

- Async stream teardown freed its object without destroying its condition
  variables or mutex. Capability rejection and partial allocation failures
  also bypassed complete cleanup.
- Async format and byte-count conversion could reach assert-based format
  helpers with invalid input, and the usbfs transfer-budget multiplication
  could overflow.
- Sync setup accepted zero transfer counts and unsafe sample geometry. Its
  firmware GPIF-size selection compared version major/minor fields
  independently, so a newer major with a lower minor could select the legacy
  packet size. The same version comparison defect existed in async setup.
- Sync setup failures after allocation called `sync_deinit()` before the
  handle's final `initialized` flag was set, leaving partial allocations and
  synchronization objects behind. `buf_ready` initialization failure was
  ignored and the condition variable was never destroyed.

## Changes

Async init now validates format, geometry, backend hooks, and capabilities
before allocation; it has a single cleanup path for partial initialization.
Normal deinit destroys its conditions and mutex. usbfs budget arithmetic is
overflow-safe. Both async and sync paths use the project's full semantic
version comparator.

Sync handles track each initialized synchronization object so both partial
failure cleanup and normal teardown release only resources that were
successfully initialized. Setup validates counts, supported formats, sample
size bounds, and rounded GPIF geometry before allocation. The public async
initializer clears output pointers and rejects zero/invalid counts before
calling a board backend.

These paths are shared by RX and TX. The source regression fixture now checks
that invalid public async stream arguments are rejected before backend entry.

## Verification status

`git diff --check` passes. Tests, builds, simulations, synthesis, and device
operations remain intentionally unrun while the requested complete source
defect sweep is still active.
