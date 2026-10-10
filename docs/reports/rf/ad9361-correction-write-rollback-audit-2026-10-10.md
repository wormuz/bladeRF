# AD9361 correction write rollback audit

## Finding

`bladerf2_set_correction()` changed live RFIC correction registers before all writes in the operation had succeeded. RX DC offset correction spans two packed registers, and the force bit was read and written only after those updates. A failure on the second packed-byte write or the force-register access could therefore return an error while leaving a partially applied correction in the running RF path. The same ordering affected single-register TX/RX phase, gain, and TX DC corrections.

## Change

The setter now reads the old correction bytes and force register before the first write. It stages the new correction bytes, writes them, and then updates the force bit. If any write reports failure, it attempts to restore the failing/previous correction bytes and, for a failed force write, the prior force register as well. A failed rollback is logged alongside the original SPI error.

SPI failures can be ambiguous, so this is best-effort restoration rather than an atomic RFIC transaction. No fault-injection test currently exercises this board-level static setter; source review and whitespace checks only. The test/build gate remains closed until the broader RX+TX audit finishes.

## Disposition

This change reduces the chance that a failed correction request leaves persistent partial state capable of producing DC or image artifacts. It does not establish a physical shielding cause or prove analog settling behavior. No test, build, simulation, synthesis, or hardware operation was run.
