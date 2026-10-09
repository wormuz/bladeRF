# NIOS peripheral SPI failure-reporting gap — 2026-10-09

## Finding and fix

The AD9361 RFFE SPI path already used bounded polling, but the separate
`PERIPHERAL_SPI_BASE` path for the VCTCXO DAC and ADF4351/ADF400x still used
vendor `alt_avalon_spi_command()` with unbounded TRDY/RRDY/TMT polling. Several
callers returned `void`; 8x16/8x32 packet handlers could report success after a
failed write. The NIOS main loop could also remain blocked indefinitely.

Commit `71fa6df2` routes those transfers through the bounded poller and returns
status from DAC/synth writes and VCTCXO reads. Trim/synth cached values update
only after transfer success. 8x16/8x32 response packets now report failure.
Legacy config writes set `NIOS_PKT_LEGACY_ERROR_MAGIC` on failure, and
libbladeRF maps that marker to `BLADERF_ERR_FPGA_OP`.

## Verification

- `host/misc/run_nios_peripheral_spi_status_test.sh`: PASS; injected 8x16 read,
  8x16 write and 8x32 write failures all return packet failure status.
- `host/misc/run_nios_legacy_error_marker_test.sh`: PASS; the host maps the
  legacy marker to `BLADERF_ERR_FPGA_OP` and leaves normal legacy writes intact.
- `host/misc/run_nios_rfpll_batch_test.sh`: PASS, including AD9361 SPI error
  response and host mapping.
- Hosted/sweep NIOS ELFs rebuilt with `-Werror`; RAM init is identical.
- Full hosted/sweep A4 seed-5 Quartus builds on `71fa6df2` passed qgate;
  qcheck is clean and sweep max-skew is 0/240.

## Limits

This closes the source-level NIOS SPI false-success/unbounded-polling gap. It
does not prove it caused the preserved xA4 NIOS no-response incident. Exact
image hardware qualification and the incident diagnosis remain open. The
current offline candidate is documented in
`adr0207-offline-chain-71fa6df2-2026-10-09.md`.
