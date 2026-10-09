# ADR-0207 event-chain candidate r5

Date: 2026-10-09

Historical r5 snapshot. Superseded as the current candidate by r6; the FPGA
reload fail-closed recovery gate was closed after this snapshot by a
4096-byte partial-transfer hardware test. See
`adr0207-r6-event-chain-release-candidate-2026-10-09.md`.

The r4 bundle pinned the older `e985a454` library and predated the RX fault
publication ordering fix. This r5 candidate refreshes the library at
`c03c1fcb`, keeps the matching no-RPATH CPython 3.14 wheel, and carries the
corrected-NIOS sweep image plus the exact hosted image that passed runtime
qualification.

Bundle: `/home/bonho/projects/bladerf/release-staging/adr0207-event-rx-2026-10-09-r5.tar.gz`

SHA-256: `79553d852fa90ad750206f81bc7c00d830f2d799437e70c7d7ded004cf6732dc`

The bundle manifest and `SHA256SUMS` contain member identities. The candidate
library reports `2.6.1-git-c03c1fcb`, was built with
`ENABLE_TEST_RX_TRANSITION_STALL_INJECTION=OFF`, has SONAME
`libbladeRF.so.2`, and no RPATH/RUNPATH. The unchanged wheel was installed
under an isolated staging directory; its extension resolved this exact
candidate library. All 21 `test_rf_event_notifications.py` tests passed.

The exact staged library was also selected by the native transition harness
for a live ten-transition paired RX_X2 cross-band smoke on the exact hosted
image. All ten completed with full event history, no unrecovered transitions,
first-read faults, WOULD_BLOCK retries, or stream overruns, followed by clean
RX1/RX2/device close. P50/P95/P99/max was
21.267/21.352/21.352/21.553 ms. The raw trace is included in the bundle.
The pre-existing 300-transition hosted qualification and three-layout
software-injected invalidation matrix are documented in
`hosted-a4-runtime-qualification-2026-10-09.md`.

The r5 bundle is a local release candidate, not final release sign-off. The
archived wideband LTE no-PSS RF-content case remains unresolved; RX1/RX2
production LTE/MIB repeatability and failed-image-load recovery remain open.
No SPI flash write was performed.
