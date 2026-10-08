# ADR-0207 exact-source A4 sweep fit, seed 7

Quartus Prime Standard 25.1 full A4 sweep build ran on the committed source
(`450e0598`) on 2026-10-08 and completed in 14:55. The RBF SHA-256 is
`adddb0c351ad52494d4df1bc28d568506fea84f8dfea73637ca1a04055c6e71e`.

## Timing

- Setup: 0 violated paths among the 20 reported; worst slack +0.266 ns.
- RX bundled-data max-skew: all 168 constrained paths pass.
- Timestamp capture bundle max-skew: +4.939 ns in the seed-5 full report;
  seed 7 also passes the overall max-skew gate.
- `qgate` timing check passes.

The prior seed-5 build had two setup violations, worst -0.144 ns, on
`fifo_writer.meta_write` into the RX META DCFIFO write pointer. Seed 7's
placement closes these setup violations without an RTL change.

## Remaining release blockers

- `qgate` still fails on 33 critical warnings.
- Post-fit Design Assistant: 725 High findings (`D101=724`, `C105=1`,
  `D103=0`). No warnings have been waived.
- Thus this RBF is not a release candidate despite timing passing. No image was
  loaded onto hardware.

Full build log: `adr0207-current-source-seed7-2026-10-08.log`.
Detailed setup paths: `adr0207-current-source-seed7-setup-paths-2026-10-08.rpt`.
