# AD9361 BIST API boundary audit (2026-10-10)

Source-only audit found three BIST setters that dereferenced `phy->spi` before validating either pointer, plus getters that dereferenced missing PHY/output pointers. A malformed or incomplete internal call could crash in the RFIC control path instead of returning a normal argument error.

The loopback, PRBS, and tone setters now return `-EINVAL` for missing PHY/SPI state. The void getters return without changing outputs when an input pointer is missing. `host/misc/test_ad9361_gain_table_fail_closed.c` now contains regression cases for each boundary and checks that invalid getter calls preserve sentinels.

`git diff --check` only. The regression source was not compiled or run because the full RX1/RX2/TX source defect sweep remains active and the user explicitly barred builds until it closes.
