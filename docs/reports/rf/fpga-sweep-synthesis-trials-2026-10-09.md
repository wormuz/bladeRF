# FPGA sweep synthesis trials — 2026-10-09

This index preserves the day's sweep timing experiments in Git so later
synthesis runs can start from known source revisions and compare measured
results. Each result below came from a full Cyclone V A4 fit. The WNS column is
the worst setup slack in the archived `sweepxA4.sta.summary`; positive slack
passes that setup check. RBF hashes identify the exact generated image.

| Trial artifact | Source revision recorded at build | Quartus seed | Worst setup slack | RBF SHA-256 |
|---|---|---:|---:|---|
| `sweepxA4-2026-10-09_07.46.32` | `5f73db58` (tree dirty) | 5 | +0.252 ns | `43f3ce8e6cb5b08f9b735997703c01c98d8c7ac5bb33fa756e914bc7ca77ae2f` |
| `sweepxA4-2026-10-09_12.25.56` | `612cc49c` (clean) | 5 | +0.435 ns | `492c5ced782f5d9ca683faedf986fc6086b994e5d5e13ecaa3597cf3caafde2c` |
| `sweepxA4-2026-10-09_13.06.20` | `d80b06ef` (clean) | 5 | +0.435 ns | `02fe7a3ee7d3d6676dd13f91e2381239e66ee858a1599cb70d28ede692b55233` |
| `sweepxA4-2026-10-09_14.27.35-seed3` | `9fdef9d0` (clean) | 3 | -0.018 ns | `15f99fb6f5be5dc87c5d1c36f43e6b684dbfbac8c2e386412fbf14afcb290480` |
| `sweepxA4-2026-10-09-14.48.36-seed5` | `9fdef9d0` (clean) | 5 | -0.016 ns | `649dabc2f4be3a513f40b3e8149b6983b95bc8bd136c119de2529fc70630e54b` |
| `sweepxA4-2026-10-09-15.10.09-pipelined-seed5` | `9fdef9d0` (dirty) | 5 | -0.346 ns | `1abf14a6c195cc2ee11836c028ea949e53c826607fed1e9f3f1cbee3fdecdc4f` |
| `sweepxA4-2026-10-09-split-seed5` | `9fdef9d0` (dirty; comparator split later committed as `e6cee6aa`) | 5 | +0.234 ns | `60aa42898b66c27e47912b0332d282f0f8783c8602ce91dcf603e3c9b24ca02b` |
| `sweepxA4-2026-10-09_16.41.17` | `bf5b4001` (clean) | 5 | +0.234 ns | `282b1668f214de981c625fd7143d5ed6b18bdfb474ad7a9bbe74d69056d3283b` |
| `sweepxA4-2026-10-09_17.34.26` | `82ae6671` (clean) | 5 | +0.234 ns | `1b59861f9361e2bf579420100a5e242f9103b61323eb7a27e58af5f852d698bb` |
| `sweepxA4-2026-10-09_18.20.36` | `71fa6df2` (clean) | 5 | +0.234 ns | `fd706fcf6010bc0bb6f8e46c74171f05965a043d1dd2633ea6a17e444e6a3a6b` |
| `sweepxA4-2026-10-09_19.04.53` | `cc38e59e` (clean) | 5 | +0.234 ns | `3e5934932bd502f41af5aac231a1f9c9d850fa26afc5310448ed6676df886308` |

## Decision and reproducibility

The selected timing fix is the split high/low comparator implemented in
`hdl/fpga/ip/nuand/synthesis/dwell_summary.vhd`, committed as `e6cee6aa`.
Its change preserves the existing registered cycle contract. The full-fit
verification, hold slack, max-skew inventory, and scope limits are recorded in
[`sweep-dwell-comparator-timing-2026-10-09.md`](sweep-dwell-comparator-timing-2026-10-09.md).
Use the committed source and the repository's Quartus build/qgate flow for the
next synthesis; do not select an image solely by its RBF filename.

The pipelined trial improved no timing: its worst setup slack regressed to
-0.346 ns. Its RBF and fitted reports remain in the local ignored synthesis
output directory, but the working-tree diff for that trial was not archived.
It is therefore historical evidence only and cannot be reproduced exactly
from this index. The final split-comparator source is committed and
reproducible.

The dated `sweepxA4-*` directories contain generated Quartus reports and RBFs
and are intentionally not Git source. The table makes their hashes and
measured outcomes durable; the HDL source revisions and this decision record
are the inputs that must be carried into future builds.

This is FPGA timing evidence only. It does not qualify RF behavior or close the
hardware RX1/RX2 event-driven release gates.
