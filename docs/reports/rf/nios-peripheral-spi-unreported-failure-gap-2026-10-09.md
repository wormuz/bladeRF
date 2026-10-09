# NIOS peripheral SPI failure-reporting gap — 2026-10-09

## Finding

The AD9361 RFFE SPI path now uses a bounded poller and returns status through
NIOS packet responses. A source audit found a separate SPI path still using the
vendor `alt_avalon_spi_command()` directly in `devices.c`: `PERIPHERAL_SPI_BASE`
operations for the VCTCXO trim DAC and ADF4351/ADF400x peripherals. The vendor
routine polls TRDY/RRDY/TMT without a deadline. Several callers return `void`,
and the 8x16/8x32 packet handlers consequently report success after these
writes. A stalled peripheral SPI core can still block the NIOS main loop and
leave the host without a response; a completed-but-failed transaction can be
reported as success.

This is a second, distinct fail-closed gap. It is not shown to be the cause of
the preserved xA4 no-response incident, and it is outside the AD9361-only SPI
regression added in bladeRF `82ae6671`.

## Required follow-up

1. Route the bladeRF-micro peripheral SPI transfers through the bounded
   poller, preserving merged chip-select semantics for VCTCXO DAC reads.
2. Return checked status from trim-DAC and ADF write functions and propagate it
   through 8x16/8x32 response status; update cached register values only after
   confirmed transfer completion.
3. Add injected timeout/error tests for each packet mapping, then rebuild both
   NIOS ELFs and the hosted/sweep images. Do not call the current offline
   candidate a stable release until this gap and the exact-image hardware gates
   are closed.

## Scope note

The bitstream, NIOS response, and host API must all agree on failure. A bounded
poll alone prevents an infinite wait but does not satisfy the public error
contract if packet handlers still emit success.
