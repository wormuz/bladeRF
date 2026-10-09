# FPGA reload recovery after a pre-backend failure

Date: 2026-10-09

## Change

Added opt-in CMake option `ENABLE_TEST_FPGA_LOAD_FAILURE_INJECTION` (default
OFF). In an injection-enabled build, `BLADERF_TEST_FAIL_FPGA_RELOAD_ONCE`
causes exactly one already-initialized xA4 reload to fail with
`BLADERF_ERR_UNEXPECTED` after the RX fault monitor has been stopped and
before `backend->load_fpga()` is called. The previous FPGA image is therefore
left untouched. Recovery now queries `is_fpga_configured()` before restoring
the RFIC. It restarts the monitor only if it was running before the reload;
if the image is not confirmed configured, the handle is demoted to
`STATE_FIRMWARE_LOADED` so normal RF operations fail closed until a fresh
image load. The test executable's `--failed-reload-only` arms the variable
only around the targeted reload, requires the injected error, verifies a
frequency update still works when the old image is confirmed, then performs a
subsequent real same-handle reload and closes.

The fault injection and its per-device one-shot latch are absent from normal
production builds. The default `host/build` production configuration rebuilt
`libbladeRF.so` and `libbladeRF_test_fpga_load` successfully with injection
disabled. A separate `host/build-fpga-failure` configured with injection ON
also built successfully.

## Hardware run

The exact runtime-qualified hosted image was used:

```text
RBF: hostedxA4-2026-10-09_08.19.04/hostedxA4.rbf
SHA-256: afb9b39272e938830714a66358faf5f9c6913a96791f57fb2ccafdd66bed3113
Device: xA4 serial f695006ba84a40daa7b777c6a6eba78
```

The `--failed-reload-only` run against the local injection-enabled library
used `BLADERF_FORCE_FPGA_A4=1` and a 90-second process timeout. Device open,
initial volatile image load, and sample-rate setup succeeded. The targeted
reload returned the expected injected `BLADERF_ERR_UNEXPECTED`; RFIC
initialization and a subsequent RX frequency update both succeeded. A real
same-handle FPGA reload then succeeded, and the test exited 0.

A second run used `--failed-reload-unconfigured-only`, which additionally
injects a host-side report that no configured image remains. The library
logged the unconfigured status, refused the RX frequency update with
`BLADERF_ERR_UNEXPECTED` because the board was in `Firmware Loaded` state, then
accepted a fresh real FPGA load and exited 0. This tests fail-closed state
handling; the FPGA itself remained configured during this synthetic status
injection. Both runs used volatile loads; no SPI flash write was performed.

## Limits and remaining gate

This proves host-side recovery after a pre-backend failure when the previous
image remains configured, and verifies host fail-closed behavior when the
configured-image query reports false. It does not simulate a USB/backend
failure after partial FPGA reconfiguration, nor a failure to reinitialize the
RFIC or restart the monitor. It verifies RF tuning and subsequent reload after
the restored-image path, but does not run a META RX stream before reloading.
Those boundaries remain open for the broader failed-image-load recovery gate.
