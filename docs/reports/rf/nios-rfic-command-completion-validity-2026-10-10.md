# NIOS RFIC command completion validity audit (2026-10-10)

Status: source-only. No build, test, simulation, synthesis, or board operation was performed.

## Defect

The RFIC status word reported only the low bit of `write_queue.last_rv`.
`rfic_queue_reset()` initializes that value to `0xFF`, so its low bit looked
like a successful command even though no queued command had retired. The host
waited for queue length zero and then treated that status bit as proof that an
RFIC write completed successfully. A NIOS reset/queue reset during command
handling could therefore make an unexecuted RFIC command appear successful.

## Fix

- NIOS queue state now tracks `last_rv_valid`, clears it on queue reset, and
  sets it only when a completed entry is retired.
- RFIC status bit 16 publishes that validity independently from the result
  bit; bits 63:17 remain reserved.
- libbladeRF rejects a drained queue unless the status says a command actually
  retired and its result succeeded.
- Added a host-side status-policy regression source for the `0xFF` sentinel,
  a successful retired command, and a failed retired command.

## Compatibility follow-up

The validity bit is absent from FPGA/NIOS images predating this change, so a
new host library must not require it from an older image. FPGA capability
`BLADERF_CAP_RFIC_WQ_RESULT_VALID` is advertised starting at v0.17.0; the
bladeRF2 host applies the strict validity check only when that capability is
present and retains the legacy success-bit check otherwise. The bladeRF2 and
bladeRF-micro NIOS image version headers now report v0.17.0. The policy
regression source covers both legacy and capability-enabled behavior.

## Verification boundary

`git diff --check` passes. The regression runner, host/NIOS builds, and device
qualification have not been run. This source change does not close the NIOS
no-response hardware diagnosis or the complete RX+TX source audit.
