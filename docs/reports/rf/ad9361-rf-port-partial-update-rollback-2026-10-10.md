# AD9361 RF-port partial-update rollback — 2026-10-10

`ad9361_rf_port_setup()` updates TX monitor power/enables before writing `REG_INPUT_SELECT`. If any SPI write failed, it returned immediately. The public port setters restored only their software-selected-port cache, so hardware could retain a changed monitor state or an ambiguously applied port write while software reported the previous selection. This could leave the RX monitor/RF route inconsistent after an I/O error and complicate DC/spur diagnosis.

The no-OS implementation now snapshots `REG_INPUT_SELECT`, `REG_ANALOG_POWER_DOWN_OVERRIDE`, and `REG_TPM_MODE_ENABLE` before a receive-port change. On any subsequent failure it attempts to restore the input-select register and the exact TXMON power/enable fields, logs rollback failures separately, and returns the original operation error. TX output port changes also restore an ambiguously written input-select value.

This closes the recoverable partial-update path when the SPI link accepts rollback writes. A persistent SPI fault can still prevent restoration; the original failure remains visible to libbladeRF. This source finding is not evidence that shielding complaints or observed spurs came from this path. Correlation still requires RF captures and register/event traces.

A related cache-state defect existed in external band control. A band update can partially change external-switch GPIOs before a later operation fails, while `current_rx_setting` or `current_tx_setting` still points to the old band. Returning to that cached band used to skip all writes, preserving the partial switch state. On any failed band application, no-OS now clears the cached setting so the next request fully reapplies the configured GPIOs and RF ports. This is recovery on the next band request; it does not claim hardware rollback during the failing request.

Commits: `e1e955322 Rollback partial AD9361 RF port changes`; `cb035cd61 Invalidate cached band after partial switch failure`.

Validation: `git diff --check` passed before commit. Builds and tests were intentionally not run because the broader RX/TX source audit is still open under the no-build instruction.
