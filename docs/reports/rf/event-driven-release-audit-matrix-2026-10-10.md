# Event-driven bladeRF release audit matrix

This matrix is the working source-audit ledger for the release objective:
deliver a stable FPGA/NIOS → libbladeRF → Python RX validity contract for
RX1, RX2, and paired RX_X2 before detector work resumes. A source review is
not runtime qualification. The user-required build/test/synthesis gate stays
closed until every source row below has a reviewed inventory and confirmed
defects are fixed. Do not use sleeps, fixed sample discard, or timeout expiry
as evidence that IQ is valid.

## Required validity contract

1. An RX transition revokes the prior certificate before any mutation that
   can alter RF state or data provenance.
2. FPGA admits samples only after the transition's required control state is
   confirmed; epoch and first-valid timestamp describe the samples admitted.
3. libbladeRF delivers no sample unless epoch, timestamp, channel mask, layout,
   and transport integrity match the active certificate.
4. Every detected integrity/status failure is durably reported before data
   delivery can resume; inability to read required status is itself a failure
   notification.
5. RX1 and RX2 share the AD9361 RX LO and common certificate. RX_X2 is valid
   only when both lane inputs meet the same boundary.
6. A later explicit transition may establish a new certificate. A diagnostic
   event about an earlier failure must not silently change the current
   certificate.

## Source audit

| Priority | Surface | Source status | Required next evidence / closure condition |
|---|---|---|---|
| P0 | RX RTL gate, lane mux, FIFO boundary, reset/error behavior | Gate placement, timestamp coupling, ABORT priority, RX_X2 all-lane condition reviewed. Reset and failure behavior only partially reviewed. | Complete reset-source/lane aggregation review; testbench covers RX1, RX2, RX_X2, reset during pending/active, ABORT/COMPLETE race, FIFO pressure, timestamp continuity, and no invalid sample after fence. |
| P0 | NIOS RX epoch command/snapshot | Version-2 atomic status/timestamp snapshot and legacy stable-timestamp fallback reviewed. | Audit command serialization, reset/re-init, stale transaction, counter wrap, completion/error publication, cancellation, and every I2C/SPI/PIO failure path. Run regression only after source defects close. |
| P0 | NIOS scheduled RX fastlock / retune2 | Active handler scope confirmed; opportunistic-preload false-failure counter fixed. Deferred RX failure event path added in bladeRF `3dbb0bbc`/`84ea1730`/`9cdd25c5`; Python event mapping `84b5280`/`1c8d015`; source harness added. | Review pending/cancel/recovery ordering and counter reset/status-unavailable semantics. Queue harness and `rx_retune_failure_notification_live` must pass after gate opens. |
| P0 | NIOS scheduled TX fastlock / TX retune gate | TX mute/abort and completion flow reviewed in slices; TX packet-length boundary fixed. | Finish every queue failure/cancel/ambiguous USB response path, gate CDC/status/reset path, and async TX callback behavior. Run controller and queue tests after gate opens. |
| P0 | AD9361 no-OS lifecycle and RF state | Many lifecycle, calibration, tracking cache, RF-port, FIR, gain, and ambiguous-SPI fixes are recorded in NOW. | Finish the full exported API/call-site inventory: validate inputs before bus access, propagate every IO result, invalidate uncertain caches, and guarantee cleanup after possibly-applied writes. Include RX and TX; reconcile every ignored-result search. |
| P0 | libbladeRF RX transition transaction | Source slices cover control-plane events, epoch snapshot, invalidation, and transition certification. | Finish begin/wait/abort/close races, timeout/error cleanup, retune/cancel interleavings, module disable/re-enable, channel provenance, and status-monitor interaction. Timeout is failure only. |
| P0 | libbladeRF sync RX parser/admission | META epoch/timestamp/channel checks and pre-copy admission are reviewed. | Audit every parser branch, message fragmentation, timestamp wrap/gap, ring/reorder overflow, event history loss, RX1/RX2 lane selection, and RX_X2 partial lane cases. Prove no write to caller buffer on rejected reads. |
| P0 | libbladeRF async RX, libusb and CyAPI | Common admission, callback ordering, recycle, length bounds, and terminal-transfer withholding reviewed. | Complete fault injection for short/oversized/failed/cancelled transfers, teardown/restart races, buffer reorder/recycle, stale callbacks, callback exceptions, and status/event ordering. |
| P0 | Python Cython wrapper | Channel/integer/enum validation and event mapping are source-audited in slices. New RX retune diagnostics added; C/Cython enum order statically matched at 50 entries. | Finish full native-call/event-drain inventory, callback concurrency, exception/teardown behavior, history-gap behavior, and local-extension/local-library provenance. Rebuild from this checkout after source gate. |
| P1 | RF content, DC/IQ correction, spurs/shielding | Active correction path and no-OS tracking state reviewed; no confirmed new xA4 correction defect in reviewed slices. | Finish writer/startup/register inventory. Then compare repeatable conducted IQ captures/FFT with identical settings; do not change correction or spur masks without RF evidence. |
| P1 | Host/package/build provenance | Python release linker rejects RPATH/RUNPATH; local build script is designed to force sibling wrapper/library. | Verify build script inputs, generated Cython source policy, extension/library hashes, installed module path, ABI/version, and release archive manifest. |

## Release qualification gates

These gates remain open until there is current evidence tied to the exact
source and image hashes. Source review or a successful offline fit alone does
not close them.

| Gate | Status | Evidence required |
|---|---|---|
| Source audit | OPEN | All P0 rows closed with reviewed inventories, fixes, and source regression coverage; then run the complete deferred test/build/synthesis suite. |
| FPGA image identity/timing | OPEN | Reproducible full fit; qgate/qcheck clean for the exact RTL/SDC; capture image SHA-256 and Quartus report set. No unrelated seed result may stand in for the release image. |
| RX1 exact-image runtime | OPEN | Exact image/library/wrapper hash tuple; event order; zero stale/invalid IQ; valid META epoch/timestamp; repeated transition/recovery and fault injection. |
| RX2 exact-image runtime | OPEN | Same evidence independently with RX2 selected; shared-LO invalidation and channel provenance checked. |
| RX_X2 exact-image runtime | OPEN | Paired-lane capture with both lane masks, same epoch boundary, no partial-lane acceptance, repeated transition/recovery and fault injection. |
| NIOS no-response incident | OPEN | Capture the exact failing command, stage, timeout, device/FPGA/firmware hashes, and transport/RFIC status on hardware; root cause and recovery proven. Do not reflash before preserving failing evidence. |
| LTE RF-content acceptance | OPEN | Repeatable known-cell capture with raw IQ and settings; PSS/PCI/MIB criteria pass after valid epoch. Distinguish invalid data-path from valid but RF-content failure. |
| Release artifact | OPEN | Versioned exact FPGA image, firmware, libbladeRF, Python extension, source revisions, hashes, install/import verification, and qualification report packaged together. |

## Work order

1. Close remaining no-OS AD9361 and NIOS caller/error-path inventory, including
   the new deferred RX-retune notification path.
2. Close remaining RTL RX/TX lane, reset, FIFO, and CDC source inventory.
3. Close native sync/async parser and transaction lifecycle inventory.
4. Close Python event/callback/runtime provenance inventory.
5. Only after 1–4 are closed, run regressions, production builds, FPGA
   simulation/synthesis, then exact-image hardware qualification.
6. Close NIOS incident and LTE RF-content gates with preserved evidence, then
   package the stable release. Detector work remains deferred until then.

## Evidence tracking rules

- Every newly found defect gets a durable NOW entry, a focused regression
  source, and a commit before moving to the next row.
- Tests/builds are not run while the source-audit gate is open, per the user's
  explicit instruction. Whitespace and read-only source consistency checks
  do not close runtime gates.
- No FPGA image is loaded and no device is operated during this source-only
  phase. Preserve existing synthesis/build artifacts and failing-device
  evidence.
- Mark a row CLOSED only when its stated evidence exists and is tied to the
  relevant source/image version; otherwise keep it OPEN or PARTIAL.
