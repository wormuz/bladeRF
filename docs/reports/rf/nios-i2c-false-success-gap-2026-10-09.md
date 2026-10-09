# NIOS I2C false-success gap — 2026-10-09

## Finding

The bounded OpenCores I2C poller already returned failure for transfer timeout
or NACK, but Si5338 and INA219 accessors hid that result. Reads returned zero
on failure and writes returned `void`; NIOS packet handlers therefore marked
failed I2C operations successful. This affected Si5338 8x8 and legacy access,
plus INA219 8x16 access.

## Change

The accessors now return `bool`; reads write through output pointers and clear
the output on failure. The 8x8, 8x16 and legacy Si5338 handlers preserve the
failure state. Existing libbladeRF 8x8/8x16 paths already map a cleared success
bit to `BLADERF_ERR_FPGA_OP`, while legacy access uses the error marker.

## Verification

- `run_nios_peripheral_spi_status_test.sh`: generic peripheral failure tests,
  Si5338 packet failure and INA219 read/write success/failure packet tests pass.
- `run_nios_legacy_error_marker_test.sh`, `run_nios_rfpll_batch_test.sh`, and
  `run_ad9361_calibration_read_error.sh` pass.
- `hdl/quartus/qcheck` is clean.
- Hosted and sweep NIOS builds both compile with `-Werror`; RAM init hashes
  match (`2a721541ef5b41da4b4f8727e85a6623ae4291a244cd6d4d47876ca78ca234b9`).
  Their NIOS ELF hashes differ due to generated build metadata.
- Quartus full fit, release bundle, paired host library/wrapper rebuild, and
  hardware qualification have not been rerun for this source change. The
  previous `71fa6df2` package is stale for this source and must not be treated
  as the current release candidate.
