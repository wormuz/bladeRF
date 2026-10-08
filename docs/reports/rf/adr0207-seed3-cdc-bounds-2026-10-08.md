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

## Release status

This is not a release candidate. `qgate` passes timing and physical CDC
inventory checks, but still rejects the image on Design Assistant findings:
724 D101 bundled-data structures and one C105 finding on
`altera_internal_jtag~TCKUTAP`. The D101 source inventory is preserved in
`adr0207-seed3-d101-inventory-2026-10-08.txt`; the audit script rejects missing,
duplicate, or unclassified structures. These findings need explicit
protocol-and-constraint disposition before qgate can accept a release.

Seed 7 with these added CDC paths was slower: setup slack reached -0.066 ns at
85°C and -0.006 ns at 0°C. Seed 3 closes both corners without changing RTL.

Full build log: `adr0207-current-source-seed3-cdc-bounds-2026-10-08.log.gz`.
