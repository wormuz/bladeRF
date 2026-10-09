# ADR-0207 offline build record — 2026-10-09

## Revisions and toolchain

- bladeRF source: `78fab4e68dce51f716d1147492e29e63baae9cd2`
- ADI no-OS submodule: `dcd92e1e02850f86700d7fb0f21c4da3a7a3b970`
- Python wrapper: `33cc48fc192011a2e2bed6782077fdb71d13451b`
- Target: bladeRF 2.0 micro xA4; Quartus Prime Standard 25.1, seed 5; CPython 3.14.4

## NIOS initialization fix

The NIOS build generated the current `nios_system_ram.hex` under
`bladeRF_nios/mem_init`, while the Qsys netlist consumes
`nios_system/synthesis/submodules/nios_system_ram.hex`. These files differed,
so a successful ELF build alone did not prove that Quartus had synthesized
that ELF into the FPGA image. Commit `78fab4e6` makes `build_bladerf.sh`
copy the selected NIOS application's generated image into Qsys's input and
fail if either file is missing. For this build, the two input files match:

```
SHA-256 49f8d31ccb589620b7a0964c30bfdf84d129e4f8a2aee35a0149a990c51999b8
```

The matching ELF and RAM-init hashes are recorded in the bundle's
`SHA256SUMS` manifest.

## FPGA synthesis and fit

Both complete A4 Quartus compilations finished successfully with zero errors.
Each fitter reported 26 warnings; the aggregate Quartus flow reported 238
warnings for hosted and 237 for sweep. The warnings were checked by qgate;
the known internal-JTAG C105 and classified CDC D101 findings are documented
in the exact DRC reports in the bundle.

| Revision | RBF SHA-256 | Minimum setup slack | Minimum hold slack | CDC result |
| --- | --- | ---: | ---: | --- |
| hosted | `c55b46f1d3484eaa32f530e6a82f9ab97f9c0580a141eb79df3f17929dfb4ca5` | +0.308 ns | +0.090 ns | qgate PASS; 12 handshake pairs |
| sweep | `16c61719839028048799d20b5d478eacf1191b673247d8375cc91ec74daf4da7` | +0.231 ns | +0.078 ns | qgate PASS; 15 handshake pairs; 240 max-skew paths, 0 violated |

The sweep post-fit max-skew report's worst slack is +3.075 ns. `qcheck` reports
clean. Full fit, STA, DRC, and qgate evidence is preserved in the bundle's
`qualification/` directory. The qgate evidence logs combine the exact
Quartus-generated fit/STA/DRC reports with the post-fit max-skew script output;
the original interactive `quartus_sh` console stream was not separately saved.

## Host library, wrapper, and software checks

`libbladerf_shared` rebuilt from the source revision above. The Cython wheel was
force-rebuilt in release mode against the matching headers/library with no
RPATH. An isolated install and import resolved `libbladeRF.so.2` to the exact
library in this bundle.

Passed checks include:

- 10 focused firmware/libbladeRF regression runners, including AD9361 SPI/
  ENSM fail-closed behavior, NIOS peripheral status, RX epoch metadata/status,
  transition policy, RX gate simulation, and sync epoch traversal;
- Python wrapper wheel tests: 31 passed;
- scanner RX transition/epoch/LTE-dispatch tests: 59 passed;
- NIOS USB transaction tests and the host `ad936x` target;
- release library/wheel loader pairing, with `libbladeRF.so.2` resolved from
  the candidate directory and no extension RPATH.

The optional hardware `rx_meta` test could not open a device (`Operation timed
out`) and did not provide hardware qualification evidence.

## Release status

This is a reproducible offline candidate, not a stable hardware-qualified
release. No FPGA image was programmed and no board reset/reload was performed.
The preserved xA4 NIOS no-response incident remains open. Exact-image runtime
qualification on RX1, RX2, and paired RX_X2, transition fault injection, and
LTE RF-content acceptance remain release gates. Do not treat positive static
timing or offline tests as proof of those hardware behaviors.

The exact payload is in `release-staging/adr0207-offline-20261009-78fab4e6/`;
verify it with `sha256sum -c SHA256SUMS` from its `bundle/` directory.
