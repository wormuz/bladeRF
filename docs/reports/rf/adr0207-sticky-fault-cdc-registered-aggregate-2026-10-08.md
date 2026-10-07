# Registered RX/TX sticky-fault aggregate CDC

## Change

The five sticky FIFO fault bits were reduced combinationally before feeding
the RX and TX two-stage system-clock synchronizers. Quartus Design Assistant
reported eight D103 structures at that boundary. Added `sticky_reduce`, which
registers the OR in the originating RX/TX sample-clock domain before the
single-bit synchronizers. The one-cycle reporting latency is safe because the
source fault vector stays asserted until an explicit clear. The independent
RX detailed-cause snapshot handshake remains unchanged.

`sticky_reduce_tb` models the source sticky register and verifies assertion,
retention through the registered reduction, and explicit clear propagation.
Both hosted and sweep QIPs include the helper.

## Evidence

- `./qsim sticky_reduce_tb`, `./qcheck`, and `git diff --check` pass.
- Quartus Prime Standard 25.1 A4 sweep, seed 5, full flow completed in 15:38.
- Design Assistant: 0 Critical, 722 High (`D101=721`, `C105=1`), 16 Medium;
  the eight prior D103 findings are absent from the report. No D103 structures
  remain for the aggregate fault path.
- All four AD9361 RX `up_xfer_cntrl` lanes have 61 source and 61 destination
  bits. `report_max_skew` finds 168 paths, zero violations; worst max-skew
  slack is +0.792 ns. RX lane worst slacks are +4.878, +5.042, +4.890, and
  +4.887 ns for channels 0–3.
- Worst setup/hold slack across the four timing corners is +0.428/+0.018 ns.
- qgate passes build, timing, RX bundle inventory, and max-skew checks. It
  remains red on the unresolved 721 D101 bits and C105 finding; no finding was
  waived.
- Seed 7 on the same RTL produced five max-skew violations, worst −1.356 ns,
  on the timestamp handshake. That run was not accepted. This showed qgate's
  earlier blind spot: a positive setup/hold summary did not cover
  `report_max_skew`. The full sweep flow now runs the persistent report after
  DRC, and qgate fails when the report is missing or contains violations.
- xA4 image SHA-256: `6a63bdbb212fd6211695e7b6e59b03cb2911cfaaef356f14de1f701d68bf5c45`.
  No image was loaded to hardware.

Timeout and fixed discard still never establish IQ validity. This closes the
identified FPGA CDC structure and strengthens static timing release checks;
it does not qualify analog settling, host IQ, or RX1/RX2 hardware behavior.

Raw seed-5 post-fit skew report:
`adr0207-sticky-fault-reduction-seed5-ad9361-rx-skew-2026-10-08.log`.
Seed-7 regression report:
`adr0207-sticky-fault-reduction-ad9361-rx-skew-2026-10-08.log`.
