# ADR-0207 matched offline chain build — bladeRF e6cee6aa

Date: 2026-10-09

## Result

An offline FPGA/NIOS → libbladeRF → CPython wheel bundle is assembled at
`/home/bonho/projects/bladerf/release-staging/adr0207-offline-20261009-e6cee6aa/bundle/`.
The SHA-256 manifest in the bundle covers the payload and qualification logs.
The host library and CLI report `2.6.1-git-e6cee6aa` and
`1.10.0-git-e6cee6aa`; the CPython 3.14 wheel is built from
python_bladerf `33cc48f` against that library and has a direct SONAME
`NEEDED libbladeRF.so.2` with no RPATH/RUNPATH.

The package includes hosted and sweep A4 RBFs, their matching NIOS ELFs, the
existing compatible FX3 image, libbladeRF, and the exact-source wheel. It is an
offline qualification bundle, not a hardware-qualified release.

## Verification

- Hosted A4 full Quartus 25.1 seed-5 fit completed successfully; qgate passed,
  with no negative slack. Worst setup/hold slack: +0.407/+0.295 ns.
- Sweep A4 full Quartus 25.1 seed-5 fit completed successfully; qgate passed,
  with no negative slack or max-skew violations. Worst setup/hold slack:
  +0.234/+0.282 ns; max-skew 0/240 paths violated.
- `python3 hdl/quartus/qcheck`: clean.
- Native `libbladeRF_test_nios_transaction`: PASS.
- Native `libbladeRF_test_sync_epoch_traversal`: exit 0. Its expected timeout
  and cursor-repair diagnostics are retained in the qualification log.
- The exact staged wheel and library were loaded together with `LD_PRELOAD`
  unset; `/proc/self/maps` showed the packaged `lib/libbladeRF.so.2` and the
  wheel's RX transition API was present.
- Scanner event/dual-epoch/LTE-dispatch tests: 54 passed.
- Python RF-event callback tests: 21 passed.
- Development loader guard with an injected stale `/usr/local` preload:
  1 passed; `.envrc` selected the sibling development wrapper/library.
- `bladeRF-cli --version`: `1.10.0-git-e6cee6aa`.

The hosted image was fitted from clean commit `e6cee6aa`. The sweep full fit
ran immediately before that commit, while the only tracked working-tree delta
was the comparator split subsequently committed unchanged as `e6cee6aa`; its
build metadata therefore identifies parent `9fdef9d0` plus a dirty tree. The
complete log and source-change report are retained. No other tracked source
changes were present during that fit.

## Payload hashes

| Payload | SHA-256 |
|---|---|
| Hosted RBF | `7d170d29f2e04e554dc0c2fce861aee2c61cd9935e7502e7d0a478f3d503ebec` |
| Sweep RBF | `60aa42898b66c27e47912b0332d282f0f8783c8602ce91dcf603e3c9b24ca02b` |
| libbladeRF.so.2 | `dcee9fde7fcd0f740f43b2a540601d869934bc2bec21c5b2283f55279299e585` |
| CPython 3.14 wheel | `148523cf290c8bda88c09f2579ab0429d37773b1868b36af085ca7c416539939` |
| Hosted NIOS ELF | `40f530cbf46bde9722fa719279813cf00e3375d9fdcf091a85353ebebaaddade` |
| Sweep NIOS ELF | `93f73b4e0af5bbc13abee4c67cb5218261ab3403d63483a8b0e1fcd28e94637c` |
| FX3 image | `d2ca89f53de7da0b190eccbae5a6aa9eb5fcb49421b30022403c213f71d77064` |

Full hashes for all payload and evidence files: `bundle/SHA256SUMS`.

## Release limits

No image was loaded and no hardware operation was performed; the connected
board's preserved NIOS nonresponse state remains untouched. Earlier RX1,
RX2, and RX_X2 qualification applies to the previously pinned hardware-tested
bundle, not these new RBF/NIOS/library/wheel binaries. Exact-image hardware
qualification remains open. The recurring NIOS ownership-mode nonresponse
cause and archived LTE no-PSS RF-content case remain open P0 release gates.
Timeouts remain failure outcomes only and do not certify IQ validity.
