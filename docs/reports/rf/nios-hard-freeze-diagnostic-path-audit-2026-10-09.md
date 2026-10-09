# NIOS hard-freeze diagnostic path audit

Date: 2026-10-09

## Finding

The RFIC INIT stage retained by bladeRF commit `aaf92b3e` is useful only while NIOS continues to service packet requests:

1. `_rfic_cmd_rd_status()` in `hdl/fpga/platforms/common/bladerf/software/bladeRF_nios/src/devices_rfic_cmds.c` packs `init_stage` into the NIOS RFIC STATUS response.
2. The host `nios_access()` in `host/libraries/libbladeRF/src/backend/usb/nios_access.c` sends a 16-byte command on `PERIPHERAL_EP_OUT` and waits for its response on `PERIPHERAL_EP_IN`; it does not read that PIO directly.
3. RF link, RX epoch, dwell, and RFIC status words are also read by NIOS firmware and returned through this NIOS packet path.
4. The FX3 vendor `BLADE_USB_CMD_QUERY_FPGA_STATUS` handler reads only `GPIO_CONFDONE`; the current vendor control API has no generic FPGA PIO/NIOS diagnostic read command.
5. The FX3 GPIO table defines FPGA configuration pins (`nSTATUS`, `nCONFIG`, `CONFDONE`), RF enable/reset controls, board ID input, and LED output. It has no assigned NIOS heartbeat or INIT-stage input.

Therefore, the current software-visible status path cannot reveal NIOS `init_stage` after the NIOS stops servicing USB packets. The status-stage change improves completed/returned INIT failures but does not diagnose the preserved hard no-response. This audit does not prove the current NIOS PC, exact stalled Avalon transaction, or root cause.

## Release implication

A future hard-freeze diagnostic must cross a path independent of NIOS execution. Candidate architectural options requiring separate design review are:

- a fabric-owned heartbeat watchdog and latched fault/status made readable through an existing host-accessible FPGA/FX3 path;
- a new FX3-visible sideband status signal, after pin/electrical and board-revision audit;
- a data-plane telemetry mechanism, only where a GPIF stream is already active.

Any implementation must define what remains readable when the NIOS instruction bus stalls, distinguish heartbeat timeout from an RFIC failure, preserve the latched last-stage marker, and fail closed for RX validity. It needs RTL/NIOS/FX3 tests plus a hardware-injected or reproducible NIOS-stall qualification. Do not add an assumed stage read through the NIOS STATUS packet and claim hard-freeze coverage.

## Evidence boundaries

This is a source-path audit only. It does not access or recover the device, test its pin state, establish whether the frozen CPU was blocked on Avalon, or qualify a new diagnostic. The known board failure remains preserved.
