# TX application callback error classification (2026-10-10)

Status: source-only. No build, test, simulation, synthesis, or board operation was performed.

## Defect

The Python async callbacks used `-1` for producer exceptions, invalid callback
lengths, and memory errors. `-1` is `BLADERF_ERR_UNEXPECTED`. When the TX
backend shut down cleanly with that stream error, `async_run_stream()` treated
it as a transport failure and published `BLADERF_RF_EVT_TX_TRANSPORT_ERROR`.
An invalid application payload could therefore be reported as a USB fault.

## Fix

- Added `BLADERF_ERR_CALLBACK` (`-20`) and its `bladerf_strerror()` text.
- Python callbacks now use that public code for callback exceptions and
  invalid producer lengths.
- The async TX runner returns the callback error to its caller but does not
  publish a transport-failure event for it. Native backend and USB errors
  retain their transport event behavior.
- Added a native source regression to assert the callback error is returned
  without a transport notification.

## Verification boundary

`git diff --check` passes. The native regression, Cython regeneration/build,
Python test suite, and hardware qualification have not run under the current
no-build source-audit gate.
