# Sweep dwell comparator timing closure — 2026-10-09

## Change

Split the `WIN_BITS` extrema and trigger comparisons in `dwell_summary.vhd` into parallel high/low-half compares, with a high-half equality tie-break. This preserves the existing registered cycle contract. The change targets the dwell-summary compare cone identified in the preceding seed-5 fit; no pipeline stage or sample/summary latency was added.

## Verification

- `dwell_summary_tb`: 7/7 cases passed.
- `dwell_summary_equiv_tb`: passed.
- `python3 hdl/quartus/qcheck`: clean.
- Full Cyclone V A4 sweep fit, seed 5: fitter successful.
- `qgate`: PASS; no negative setup/hold slack, no max-skew violations, CDC inventory and classified DRC dispositions accepted.
- Worst setup slack: +0.234 ns (slow 1100 mV, 85 C, RX LVDS divider clock).
- Worst hold slack: +0.282 ns (same slow model, system PLL output clock).
- Max-skew inventory: 240 paths, 0 violated.
- RBF SHA-256: `60aa42898b66c27e47912b0332d282f0f8783c8602ce91dcf603e3c9b24ca02b`.
- Full log: `hdl/quartus/work/bladerf-micro-A4-sweep/logs/sweep-split-seed5-20261009.log`.
- Archived fitted reports and RBF: `hdl/quartus/sweepxA4-2026-10-09-split-seed5/`.

## Scope and release status

This closes the previously failing A4 sweep timing gate for this source and seed. It is a static FPGA build result; it does not establish hardware RF performance or qualify the complete FPGA/libbladeRF/Python release. Do not load the image onto the attached board as part of this result. Hosted timing and hardware RX1/RX2 transition qualification remain separate release gates.
