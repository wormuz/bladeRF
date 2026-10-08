# xA4 erased calibration records and startup fallback — 2026-10-09

## Source-path finding

The startup warnings are explained by an erased SPI-flash calibration page,
not by a failed NIOS control transaction. FX3 reads the calibration page into
its `glCal` cache during startup (`NuandReadCalTable`). libbladeRF requests the
cached page and looks up binary key/value records `B` (FPGA size) and `DAC`
(VCTCXO trim). The saved read-only page contains `0xff`, so
`binkv_decode_field()` returns `BLADERF_ERR_INVAL`; the public log renders
that status as “Invalid operation or parameter.” The missing key lookup is
distinct from the historical NIOS first-request nonresponse.

The FPGA-size lookup is best-effort during board initialization. If the FPGA
is already configured, the board continues; when an image must be loaded and
the size is unknown, libbladeRF tries each known filename and validates the
image against the device before programming it. On this xA4 the saved
autoload page decompresses to a valid image and the currently loaded candidate
reports FPGA v0.16.1.

The VCTCXO path explicitly falls back to DAC trim `0x1ffc`, writes that value
to the trim DAC, and continues initialization. This is the current runtime
behavior; no factory value can be recovered from the erased calibration page.
The source audit and hardware probes performed for this report are read-only
with respect to SPI flash. No flash write or FPGA flash operation was run.

## Disposition

Classify both warnings as missing board calibration records with documented
software fallbacks. They do not explain the lost NIOS first-request state and
do not invalidate the event-driven RX qualification. The remaining board
maintenance decision is whether to leave the characterized fallback in place
or restore factory calibration through an authorized calibration procedure;
do not write flash based on a guessed trim value. The original NIOS failure
cause remains unknown and requires the prepared first-request probe before
any FPGA reload if it recurs.
