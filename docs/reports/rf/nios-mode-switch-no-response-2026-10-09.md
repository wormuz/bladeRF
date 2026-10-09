# NIOS mode-switch nonresponse recurred during event-transition guard probe

Date: 2026-10-09 10:14 EEST  
Device: bladeRF 2.0 micro xA4, serial `f695006ba84a40daa7b777c6a6eba78`

## Observation

The production r9 mode-guard probe was rerun after a clean build. USB open succeeded and libbladeRF printed the known erased-calibration-record warnings. The process then blocked for about 55 seconds with negligible CPU and its SuperSpeed USB handle open while switching RFIC ownership from Host to FPGA/NIOS. I interrupted only the host probe. It had not reached the transition-guard assertion, so this run does not test that assertion.

Before attempting recovery, the existing one-shot NIOS boot probe sent exactly one legacy version-byte request. USB OUT succeeded (16 bytes, 0.057 ms); the NIOS IN response timed out after 250.382 ms (`status=-7`). The bladeRF remained enumerated as `2cf0:5250`, SuperSpeed 5 Gbps, serial unchanged. Kernel log had no device-specific event in the queried interval. Raw evidence: `nios-mode-switch-no-response-2026-10-09.log`.

## Source-path assessment

The failed mode switch invokes the NIOS `BLADERF_RFIC_COMMAND_INIT` path. In `devices_rfic_cmds.c`, `_rfic_initialize()` clears RF control, releases reset, enables the AD9361 interface, then runs `ad9361_init()` and per-direction setup before returning the command response. A subsequent source audit corrected the initial waitrequest hypothesis: `axi_ad9361.s_axi_clock` is `system_clock`, and ADI `up_axi.v` synthesizes a response after 15 count cycles if the internal register block does not acknowledge. Reads return `0xDEADDEAD`; writes complete with AXI OKAY and expose no timeout flag. The API path does not currently propagate these outcomes as a reliable NIOS command error. The observed no-response could still arise from later unbounded waits or packet servicing. This run captured neither `init_stage` nor the NIOS program counter, so root cause remains unproven.

## Handling

No FPGA reload, USB reset, power cycle, or subsequent RF operation was performed after the timeout. The original volatile failure state is preserved for the next diagnostic step. Do not use the normal runtime qualification runner or reload candidate images until the NIOS request/response state and host/kernel evidence are collected.

This reproduces the known class of NIOS nonresponse after a mode-transition attempt but does not establish root cause. The r9 RX1/RX2/RX_X2 1,000-transition soaks completed before this event; this failed probe is not included as an RF transition result.
