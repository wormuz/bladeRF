# ADR-0207 current-source A4 sweep fit, seed 3

Quartus Prime Standard 25.1 full A4 `sweep` fit completed on 2026-10-08 with
the ADI RX/TX control, clock-monitor, and status CDC bounds enabled. The RBF
SHA-256 is `f53cd1c1fbdc8176c40397d1881a70b5081e8d489854928fb7732dc20ab4fa12`.
The image was not loaded onto hardware.

## Timing and fitted CDC evidence

- Worst 85°C setup slack: +0.412 ns; TNS 0.
- 240 max-skew paths, 0 violated; worst slack +3.736 ns.
- ADI `up_xfer_cntrl`: 9/9 fitted bundles paired (four RX channel, four TX
  channel, TX common).
- ADI clock-monitor snapshots: 2/2 paired.
- ADI transfer-status snapshots: 6/6 paired.
- No ignored constraints, empty collections, or non-waived missing paths.

## FPGA fit-gate status

`qgate` passes this fit. It requires the exact D101 inventory and the fitted
max-skew/bundle evidence above; it does not lower Design Assistant thresholds
or alter the report. The 724 D101 structures classify into 393 held-handshake,
128 tamer-payload, 129 ADI control, 10 ADI status, and 64 clock-monitor source
bits. The parser fails on missing, duplicate, or unclassified structures.

The single C105 finding is the generated `altera_internal_jtag~TCKUTAP` clock,
outside the RF data/transition path. A trial promotion to a global network
removed C105 but created a -0.831 ns JTAG TDO output violation and degraded RX
setup; that assignment was reverted. qgate accepts only this exact C105 node
and fails if the count or endpoint changes. D103 remains zero. The failed
promotion and path-level diagnosis are preserved in
`adr0207-current-source-seed3-jtag-global-2026-10-08.log.gz` and
`adr0207-jtag-global-seed3-path-diagnostic-2026-10-08.log.gz`.

This closes the FPGA static/fit gate, not the stable release of the whole
firmware/libbladeRF/wrapper chain. Hardware qualification of RX1 transitions,
including no stale-epoch leakage and the LTE known-cell return tests, remains
required; RX2 qualification follows RX1.

The D101 source inventory is preserved in
`adr0207-seed3-d101-inventory-2026-10-08.txt`; qgate output is in
`adr0207-seed3-qgate-2026-10-08.txt`.

Seed 7 with these added CDC paths was slower: setup slack reached -0.066 ns at
85°C and -0.006 ns at 0°C. Seed 3 closes both corners without changing RTL.

Full build log: `adr0207-current-source-seed3-d101-disposition-2026-10-08.log.gz`.
