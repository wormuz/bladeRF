# AD9361 RX control-bundle CDC constraints

## Finding

The four AD9361 RX `up_xfer_cntrl` data buses cross from the NIOS control
domain into separate ADC-clock domains. The project SDC found 61 active source
and destination bits per lane, but Quartus ignored each `set_max_skew` because
ADI's inherited `ad_axi_ip_constr.sdc` also applied a broad false path to all
`up_xfer_data -> d_data_cntrl` paths. Nonempty collections alone had hidden
that conflict.

## Change

The inherited false path now excludes only the four RX destination bundles;
the remaining ADI transfers retain their existing false-path treatment. The
board SDC applies paired per-channel max/min delay, 6.4 ns max skew, and
6.4 ns net-delay constraints. `qcheck` audits both SDCs and rejects a return
to the broad cut. The persistent `report_adc_xfer_cdc.tcl` inventories the
fitted source/destination widths and runs Quartus `report_max_skew`.

## Evidence

- Quartus Prime Standard 25.1 A4 `sweep`, full fit, seed 7: successful; total
  build time 15:43.
- All four RX bundles paired at 61 active bits per endpoint.
- `report_max_skew` worst-case slack: RX channels 0–3 were +4.855, +4.964,
  +4.853, and +4.907 ns against the 6.4 ns limits.
- No ignored RX skew or RX no-path assignment. The build/SDC/timing checks
  pass, but full qgate correctly remains red on the Design Assistant report:
  D101=720 and D103=8 high violations (plus C105=1). D101 includes 112 RX
  ADI control-bus bits, 16 TX ADI control-bus bits, 389 Nuand bundled-
  handshake bits, and 202 NIOS/time-tamer paths. The four RX bus bounds are
  active; their D101 classification remains visible and unwaived. D103
  identifies eight RX/TX FIFO sticky-fault status bits and remains open
  despite the RX snapshot simulation.
- `qcheck`, the RX fault-cause CDC simulation, and `git diff --check` pass.
- The generated xA4 image SHA-256 is
  `b26a94f964e8c88cc3883d78d986e59354834c110def4cf4acb59389f39f60d9`.
- The generated xA4 image SHA-256 is
  `b26a94f964e8c88cc3883d78d986e59354834c110def4cf4acb59389f39f60d9`.

This is a static timing/CDC improvement only. No board image was loaded, and
it does not constitute analog retune or RX1/RX2 hardware qualification. It
does not change the validity contract: timeout or a fixed discard never
establishes valid IQ.

Raw post-fit timing output: `adr0207-ad9361-rx-bundle-timing-2026-10-08.log`.
Design Assistant output: `adr0207-ad9361-rx-bundle-design-assistant-2026-10-08.log`.
