# FPGA reload close monitor instrumentation (2026-10-09)

The reload-then-close stall remains unreproduced. Before another reload, libbladeRF now captures the RX fault monitor's active stage when `pre_close` requests stop, the elapsed time in that stage, and the thread join duration. Stages distinguish RFFE status, NIOS link status, NIOS loss count, and each of the three AD9361 status reads. The stage snapshot is protected by the monitor mutex; instrumentation does not cancel or interrupt USB I/O and does not change retry/deadline behavior.

When debug logging is enabled, look for:

```text
rx_fault_monitor_stop: stopping at stage=<stage> stage_elapsed_us=<n>
rx_fault_monitor_stop: join_complete elapsed_us=<n>
```

Build verification: `cmake --build host/build -j2` completed, followed by a focused rebuild of `libbladerf_shared`; `git diff --check` passed. No live close or FPGA reload was run after this instrumentation, so it provides a targeted capture point but does not establish the cause or fix the stall.

Commit: `ca6e133f Instrument RX monitor close join`.
