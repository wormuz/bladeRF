# AD9361 independent-FDD ENSM mode audit

## Finding

`ad9361_set_en_state_machine_mode()` selected `ENSM_MODE_PINCTRL_FDD_INDEP`, then called `ad9361_set_ensm_mode()` before updating `pdata->fdd_independent_mode`. The helper derives `FDD_EXTERNAL_CTRL_ENABLE` from that field, so it programmed the previous mode while the API subsequently cached the requested mode. This allowed software state and the AD9361 register state to disagree. The IIO attribute setter duplicated the same ordering and its own cache update.

## Fix

The public API now stages the requested independent-FDD flag before programming the mode and restores the previous cache if the mode-register programming fails. If the subsequent ENSM state request fails, the cache remains aligned with the successfully committed external-control register bit. The IIO setter now parses its existing strings to the public `ENSM_MODE_*` values and delegates to the same API.

Added SPI-register-image regression coverage for entering independent FDD and returning to regular FDD. The test was authored but not executed under the current source-audit gate.

## Disposition

Source fix committed in no-OS `dd2f8dbbc` (`Fix AD9361 independent FDD mode programming`). `git diff --check` passed. No build, test, simulation, synthesis, or hardware operation was run. This is a concrete control-state correctness defect relevant to RF spur/validity investigations; it does not demonstrate that shielding complaints are caused by this path or that software can replace physical shielding.
