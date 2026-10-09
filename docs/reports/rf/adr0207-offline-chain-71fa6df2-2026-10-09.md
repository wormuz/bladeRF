# ADR-0207 offline event-chain build — bladeRF 71fa6df2

Date: 2026-10-09

## Result

Built a matched offline candidate from bladeRF `71fa6df2`, no-OS submodule
`b7e1fe46e`, and python_bladerf `33cc48f`. It contains the A4 hosted/sweep
images, matching NIOS ELFs and RAM init, libbladeRF
`2.6.1-git-71fa6df2`, a CPython 3.14 no-RPATH wheel, compatible FX3 image,
and full build/test evidence.

This is an offline candidate, not a stable or hardware-qualified release. The
exact images have not been loaded because the xA4 remains preserved after its
NIOS no-response incident. Incident root cause, exact-image runtime
qualification, and archived LTE no-PSS RF-content acceptance remain open.

## Error-reporting fixes

This source closes two distinct NIOS SPI gaps:

1. Direct AD9361 RFFE register and fastlock/PLL reads now propagate SPI errors
   rather than returning zero or stale status as success (`82ae6671`).
2. VCTCXO DAC and ADF4351/ADF400x peripheral SPI now uses the same bounded
   poller. 8x16/8x32 packet responses carry failure status; legacy config writes
   use a response error marker that libbladeRF maps to `BLADERF_ERR_FPGA_OP`.
   Cached trim/synth values update only after successful SPI transfer
   (`71fa6df2`).

Regression tests inject packet-level peripheral failures and verify failure
status for reads/writes, plus host-side legacy error-marker mapping. This does
not prove which mechanism caused the preserved hardware no-response event.

## Build and verification

- Hosted and sweep A4 seed-5 full Quartus 25.1 builds completed with 0 errors.
  qgate passed for both. Hosted worst setup/hold slack: +0.308/+0.090 ns.
  Sweep worst setup/hold slack: +0.231/+0.078 ns; post-fit max-skew found
  240 paths with 0 violations. `python3 hdl/quartus/qcheck` is clean.
- Both NIOS ELFs were rebuilt with `-Werror`; their RAM init files match
  (SHA-256 `d19f519ed4b5171de2c1d700a9a6a9b11e7af9642f22ad83a1153cd4c17684b5`).
- NIOS peripheral packet failure test, legacy host marker mapping, AD9361
  RFPLL packet/host mapping and retune queue tests pass. libbladeRF NIOS
  transaction and sync epoch traversal tests exit successfully.
- The host library version stamp is `2.6.1-git-71fa6df2`.
- The wheel was rebuilt in release mode. `readelf` confirms
  `DT_NEEDED=libbladeRF.so.2` and no RPATH/RUNPATH. Wrapper tests pass (31).
  Scanner transition/retune/dual-epoch/LTE-dispatch tests pass (59), developer
  stale-preload guard passes (1), and `/proc/self/maps` confirms that the
  wheel loads exactly the paired library and exposes transition/event APIs.

Full hosted/sweep Quartus logs, fit/STA/DRC reports, qgate results, qcheck,
source ELFs, and test logs are in the bundle.

## Artifact SHA-256

| Artifact | SHA-256 |
|---|---|
| Hosted A4 RBF | `a6895c6ccaf44440609411bfe260310257da17e3b3f0a5f37261e0ddce619be0` |
| Sweep A4 RBF | `fd706fcf6010bc0bb6f8e46c74171f05965a043d1dd2633ea6a17e444e6a3a6b` |
| Hosted NIOS ELF | `eec57eb695f28dbb4ef18428dba01b1234fb4a5a88338acfc980920062132ca5` |
| Sweep NIOS ELF | `a9bb7385cd34aa380523321534cf30754ec5ea905f2f0daf78f1220694d78085` |
| libbladeRF.so.2 | `ef893ba762dd42d4db5a113b00f9c857caaab8026acae80f5e4e36d48bd34e66` |
| CPython 3.14 wheel | `0faafa01f9a225b93fef735517844fcc78961504e3bd747eac81ef407858f26f` |

Full payload hashes are in bundle `SHA256SUMS`.

## Remaining release gates

1. Preserve and complete diagnosis of the xA4 NIOS no-response incident before
   any reset or FPGA reload.
2. Qualify these exact hosted/sweep RBF, NIOS, library and wheel artifacts on
   RX1, RX2 and RX_X2, including transition and transport-fault cases.
3. Resolve and repeat the archived LTE no-PSS RF-content acceptance case.

Timeouts remain failures; static timing or event completion alone does not
certify runtime RF sample quality.
