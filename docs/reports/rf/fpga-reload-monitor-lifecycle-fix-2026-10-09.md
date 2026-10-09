# Same-handle FPGA reload RX monitor lifecycle fix (2026-10-09)

## Finding

The same-handle reload path called `_bladerf2_initialize()` a second time. That function starts the RX fault monitor, but `bladerf2_rx_fault_monitor_start()` previously initialized the monitor mutex and condition variable and created a thread unconditionally. The existing thread could therefore be orphaned while its synchronization objects were reinitialized. On close, only the newest thread was stopped/joined; the stale thread could retain or wait on `dev->lock`. This matches the instrumented failure: monitor join returned in about 99 ms, but close never passed its initial board-close entry. A second attempt showed the same ordering and then crashed; ptrace policy prevented attaching gdb to that process.

## Fix

Before reloading an already configured FPGA, stop/join and destroy the existing monitor synchronization objects. Successful reload initialization then creates exactly one fresh monitor. `start()` is also idempotent when a monitor is already active. If FPGA loading fails and the previous RFIC controller is successfully restored, restart the monitor; if restoration fails, report that failure and leave it stopped rather than attach it to an unknown RFIC state.

## Hardware validation

Using the rebuilt local `bladeRF-cli` and local `libbladeRF`, one same-handle reload of the volatile sweep RBF (`43f3ce8e6cb5b08f9b735997703c01c98d8c7ac5bb33fa756e914bc7ca77ae2f`) completed. The pre-reload monitor joined in 22 us; the new monitor joined during close in 11 us. Both RX/TX sync teardown, both module-disable operations, RFIC standby, board close, and backend close completed. CLI exited 0. No SPI flash write was performed. Full trace: `/home/bonho/projects/sdr-scanner/docs/reports/rf/fpga-reload-close-lifecycle-fix-20261009.log`.

Earlier instrumented traces: `/home/bonho/projects/sdr-scanner/docs/reports/rf/fpga-reload-instrumented-close-20261009.log` (monitor join 99 ms, then close stalled) and `/home/bonho/projects/sdr-scanner/docs/reports/rf/fpga-reload-close-gdb-capture-20261009.log` (same ordering, process crashed before attach; gdb attach was blocked by ptrace policy). The code-level duplicate-start defect is directly established; the single successful same-handle run validates the fix path but is not a repeated-soak qualification.

Build: full host build passed before diagnostic instrumentation; after the lifecycle change, `libbladerf_shared` and `bladeRF-cli` rebuilt successfully. `git diff --check` passed.

Commit: `e4192c57 Stop RX monitor across FPGA reload`.
