# ADR-0207 full xA4 fit from current committed source

Date: 2026-10-10

## Result

The exact current A4 source completed a clean full Quartus sweep build with
seed 1. Total build time was 21:57. Quartus A&S reported 0 errors and 108
warnings; Fitter, Timing Analyzer, Assembler, and Design Assistant all
completed with 0 errors. `hdl/quartus/qgate` passed every release gate and
`hdl/quartus/qcheck` reported clean.

The qgate evidence includes all current CDC inventories (time-tamer 2,
handshake 13, ADI `up_xfer` 9, and DCFIFO pointers 10), all 720 D101 findings
classified, exact disposition of the internal JTAG TCK C105 finding, all
fitted max-skew paths within bounds, and no negative timing slack. The worst
multicorner setup/hold slack is +0.095/+0.065 ns; the slow 85 C `pll_sclk`
setup slack is +0.122 ns. This is a narrow but positive static timing margin.

## Image identity

- Main repository source snapshot: `bb94ce15` (the only change after the
  hardware source commit `1a27e6ba` was documentation; the ADI no-OS
  submodule is `e8b182b3546ff38aea610d27a83ade00e2d83fb6`).
- RBF SHA-256:
  `1f11317d53eeb09ea06c5ae963ef693f2ce5d013a2257e179b22cd9061b2aae5`.
- RBF archive: `hdl/quartus/sweepxA4-2026-10-10_01.42.49/sweepxA4.rbf`.
- Full raw log:
  `hdl/quartus/work/bladerf-micro-A4-sweep/logs/sweep-full-seed1-commit-1a27e6ba-20261010.log`.
- Qgate output was clean across all checks; qcheck passed.

Generated NIOS BSP headers contain `AXI_AD9361_0_BASE=0x10000`,
`RFIC_AXI_TIMEOUT_STATUS_BASE=0x9740`, and
`RFIC_AXI_TIMEOUT_CLEAR_BASE=0x9760`, confirming the regenerated Qsys/BSP
addresses used in this fit.

The image was not loaded onto hardware. The preserved NIOS no-response state
and exact-image RX1/RX2/RX_X2 runtime qualification remain open. A subsequent
ADI clock error-propagation fix is being prepared and will require a fresh
NIOS/FPGA build; this image is therefore a verified build artifact, not the
final release candidate.
