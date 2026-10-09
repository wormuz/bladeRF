# Bound NIOS AD9361 SPI status polling

Date: 2026-10-09

## Finding

The generated Intel HAL implementation of `alt_avalon_spi_command()` polls
the Avalon SPI `TRDY`/`RRDY` status in its transfer loop and `TMT` after the
transfer with no timeout. The NIOS RFIC initialization path calls the AD9361
SPI callbacks synchronously from the command handler. If the RFFE SPI core
stops advancing or reports an impossible status sequence, NIOS can remain in
the driver forever and stop servicing the command UART, producing a USB OUT
with no corresponding IN response.

This is a concrete unbounded wait in the affected call path and a plausible
mechanism for the preserved NIOS nonresponse. It is not proven to be the cause
of that incident: the failed board state did not capture SPI status or NIOS
program counter, and no hardware recovery or reproduction was performed.

## Change

The bladeRF2 NIOS firmware now uses a tracked `bladerf_rffe_spi_command()` for
all four direct AD9361 RFFE SPI call sites (read, write, and atomic read/modify/
write). It preserves the HAL transfer behavior but applies one finite
1,000,000-status-poll budget to the complete transaction, detects the SPI
overrun/underrun status bits, releases chip select and clears sticky SPI error
bits on failure, and returns `-1`. Existing checked ADI SPI callbacks then
propagate that failure into no-OS/NIOS command status. The unrelated peripheral
SPI uses retain the vendor driver.

Expiry is a failure watchdog only; it does not make RF state or IQ valid.

## Verification

- bladeRF2/sweep NIOS ELF rebuilt with the Quartus 25.1 NIOS toolchain and
  `-Werror`; ELF SHA-256:
  `93f73b4e0af5bbc13abee4c67cb5218261ab3403d63483a8b0e1fcd28e94637c`.
- Disassembly contains the bounded RFFE SPI routine and the AD9361 callbacks
  call that routine. `alt_avalon_spi_command()` remains in the image only for
  other peripheral SPI users; no direct `RFFE_SPI_BASE` call remains.
- `python3 hdl/quartus/qcheck`: clean.
- `git diff --check`: clean.
- No Quartus fit, FPGA load, USB reset, board command, or RF test was run.

The no-response root cause and the exact limit's physical timing remain open
hardware-qualification items. The software change only ensures the affected
SPI polling path has a finite failure outcome.
