# FPGA reload recovery after a pre-backend failure

Date: 2026-10-09

## Change

Added opt-in CMake option `ENABLE_TEST_FPGA_LOAD_FAILURE_INJECTION` (default
OFF). In an injection-enabled build, `BLADERF_TEST_FAIL_FPGA_RELOAD_ONCE`
causes exactly one already-initialized xA4 reload to fail with
`BLADERF_ERR_UNEXPECTED` after the RX fault monitor has been stopped and
before `backend->load_fpga()` is called. The previous FPGA image is therefore
left untouched. The existing recovery code reinitializes the RFIC and starts
the monitor again. The test executable's `--failed-reload-only` option arms
the variable only around this reload, requires the injected error, performs a
subsequent real same-handle reload, and closes the device.

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

Command ran `libbladeRF_test_fpga_load --failed-reload-only` against the local
injection-enabled library with `BLADERF_FORCE_FPGA_A4=1` and a 90-second
process timeout. Outcome: device open and initial volatile image load
succeeded; RX sample-rate setup succeeded; the targeted second reload
returned the expected injected `BLADERF_ERR_UNEXPECTED`; recovery logged
successful AD9361 initialization; a subsequent actual same-handle FPGA reload
succeeded; the program printed
`Passed failed-reload recovery and subsequent reload test!` and exited 0.
The API call to reload uses the volatile backend path; no SPI flash write was
performed.

## Limits and remaining gate

This proves host-side recovery after an FPGA load failure known to occur
before any image bytes reach the backend. It does not simulate a USB/backend
failure after partial FPGA reconfiguration, nor a failure to reinitialize the
RFIC or restart the monitor. Those failure modes still need bounded handling
and qualification before the broader failed-image-load recovery gate is
closed. The test also checks successful recovery through a subsequent real
reload/close, not through a live RX stream after recovery.
