# IIO GPO mask cache after partial selector failure

`set_gpo()` writes the requested manual output mask to
`REG_GPO_FORCE_AND_INIT`, then reads `REG_EXTERNAL_LNA_CTRL` and may write its
manual selector. The software mask cache was committed only after all of those
steps. If the first write succeeded and the later read/write failed, the call
returned an error with the new GPO bits already active but the cache still at
the old mask. A later update to one pin rebuilt the full register from that
stale mask and could silently undo the previously applied pin.

The mask cache now advances immediately after the GPO register write succeeds.
The selector operation still reports its own failure, while retries use the
mask that the GPO register accepted. `git diff --check` only; no build or test
was run under the active source-audit gate. Qualification should inject failure
at the selector read/write, then update another pin and verify the first pin
remains applied.
