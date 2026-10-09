# ADR-0207 clock-constraint and AD9361 fail-closed build — 2026-10-09

## Source pair

- bladeRF: `ab6ab456` (`Fix ignored peripheral generated clocks in SDC`), with parent commit `07bbbd52` carrying the no-OS submodule update.
- ADI no-OS submodule: `b7c41e634`.
- Python wrapper: `33cc48f`.
- Target: bladeRF 2.0 micro xA4; Quartus Prime Standard 25.1, seed 5; CPython 3.14.

## Changes

Quartus had ignored three `create_generated_clock` constraints for the I2C,
peripheral SPI, and AD9361 SPI clocks because their `-source` arguments
selected a clock object instead of a physical source node. The SDC now uses
`[get_nodes $system_clock]`. `qgate` rejects the previous ignored-generated-
clock diagnostic, and `qcheck` contains a regression proving that diagnostic
fails closed. The full compile now derives the three generated clocks; there
are no ignored `create_generated_clock` or derived-master-clock diagnostics.

The no-OS AD9361 RSSI/AuxADC setup, calibration, and measurement paths now
propagate SPI errors instead of returning false success. Injected SPI failures
are covered by `host/misc/run_ad9361_gain_table_fail_closed.sh` and pass.

## Full FPGA builds

Both complete A4 builds and post-fit checks completed with zero errors and
passed qgate. The total warning count is 181 for each image. This total spans
Analysis & Synthesis (110 sweep / 111 hosted), Fitter (15), STA (1), and DRC
(55 sweep / 54 hosted); the hosted/sweep difference is one synthesis warning
and one DRC warning. The A&S diagnostics include optimized-away Wishbone FIFO
RAM nodes, unused or unassigned RTL objects, constant expressions, and
sensitivity-list diagnostics. DRC also reports existing clock-usage, reset
synchronization, and CDC classifications. These counts are not a blanket
waiver: the ignored generated-clock class was a real constraint defect and is
now fixed and gated. Four post-fit `set_max_skew` no-path notices refer to
same-output-clock-to-port constraints; qgate classifies these separately.

| Image | RBF SHA-256 | Setup slack | Hold slack | ADC transfer CDC |
| --- | --- | ---: | ---: | --- |
| hosted | `51f8a792f3067af278d75c0a5ecd8b4c049a7ca6aa93a9f757b637e1bd893ccf` | +0.741 ns | +0.184 ns | qgate PASS |
| sweep | `c3994f57a44cce182f575f19970f1ed8c901ba48651aa33ff6bb138652b09c52` | +0.107 ns | +0.220 ns | 4 RX bundles PASS; 240 max-skew paths, 0 violations |

Both NIOS ELFs were built with `-Werror`; both generated Qsys RAM inputs match
the freshly built image at SHA-256
`4a615190fabafeb1cff391eef9e5c81ffe47f6cd8247b2607c74dcccbb078dd1`.
ELF hashes: hosted `e6bdb77180a9eb018609753131d3e9fd70ffb2500dd15c7f244363a81e5eb7f7`,
sweep `c0f1cad058ac5b4781c70bc70bd3c1f82adc7cf2b458c4ecbcc968fa7ab2bd48`.

Full console logs are preserved with the offline bundle in `qualification/`.
The local source logs were `/tmp/adr0207-rssi-hosted-full-fit.log`,
`/tmp/adr0207-rssi-sweep-full-fit.log`, and
`/tmp/adr0207-rssi-sweep-adc-cdc.log`.

## Host chain checks

The reconfigured library reports `2.6.1-git-ab6ab456`; its SHA-256 is
`590fd21910341a792efa93d5dd9d8f36d13c2b1d8b74ed7424dc104a92159297`.
The CPython 3.14 wheel was rebuilt against that source and has SHA-256
`c0c3cce0743b6bc6784d5ff57fd6838272b1c8c1f7d5f56feac1b1b4e07870cd`.
The installed extension has no RPATH/RUNPATH and was tested outside its source
checkout with the paired library. Python wrapper tests passed 31/31 and the
scanner RX transition/epoch/LTE dispatch tests passed 59/59. Native NIOS
transaction and sync epoch traversal tests also passed.

## Release state

This remains an offline candidate. No FPGA image was loaded and no device
reset was performed. The preserved xA4 NIOS no-response incident, exact-image
runtime qualification on RX1/RX2/RX_X2, and LTE RF-content acceptance are
still release gates. Static timing and offline tests do not close them.
