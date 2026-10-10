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
| P0 | RX RTL gate, lane mux, FIFO boundary, reset/error behavior | Gate placement, timestamp coupling, ABORT priority, and RX_X2 all-enabled-lane condition reviewed. The gate and FIFO writer each register sample data, so a sample already in flight at ARM may finish transport under the old epoch tag; the host revokes admission before ARM, waits for FPGA PENDING before changing LO, and rejects old-epoch packets after the new certificate. Do not independently mask sample FIFO writes: metadata and sample FIFOs use separate writer state and could become misaligned. Reset source is traced through `sys_reset_async → sys_reset_pclk → RX reset synchronizer`; runtime reset/recovery and exact-image behavior remain unverified. | Integration regression must prove an in-flight pre-ARM sample retains the old epoch and is never exposed after success; no new-epoch tag is accepted before first-valid boundary. Cover RX1, RX2, RX_X2, reset during pending/active, ABORT/COMPLETE race, FIFO pressure, timestamp continuity, and both sample/META alignment. Run only after source audit is complete. |
| P0 | NIOS RX epoch command/snapshot | Version-2 atomic status/timestamp snapshot and legacy stable-timestamp fallback reviewed. The scheduled-retune `time_tamer_read()` byte sequence uses the RTL's address-zero CDC snapshot, so it does not tear across counter rollover. A separate fail-open was found: with `ts_reset` deasserted and `ts_clock` stopped, Avalon waited forever for snapshot ack. FPGA now releases the bus after a bounded system-clock watchdog and reports snapshot-invalid; NIOS timestamp reads propagate failure. The AD9361 fastlock activation path also ignored the result of external RFFE SPDT selection; selection now verifies the PIO output readback and fails retune if unavailable or mismatched. A wider audit found `CHECK_BOOL` was used on `bool` firmware helpers even though it only treats negative integer statuses as failure; `false` was therefore accepted in initialization, standby, and RF mutator paths. Added `CHECK_TRUE` and corrected those sites. RFFE read APIs now fail when the PIO is absent; all write paths verify output readback with physical input bits masked out. The general control PIO now verifies its output loopback and propagates failure through 8x32, legacy, and compatibility packet interfaces. A queued RFIC command is now revalidated at execution time because an earlier queued INIT OFF can invalidate PHY state after enqueue; unknown command IDs also fail safely instead of dereferencing a null descriptor. This audit found the 32-bit NIOS packet AXI address was narrowed to `uint16_t` at `adi_axi_read/write`, silently aliasing addresses above 64 KiB. Signatures now preserve the wire width and reject out-of-range values before MMIO; host/headless no-OS adapters check base-plus-offset overflow. The RFIC command decoder also masked reserved command/channel address bits, so malformed addresses could alias a valid operation; it now rejects reserved bits on both read and write paths, including the PC simulation stub. Added boundary regression sources for both address formats. Another lifecycle pass found RFDC calibration history survived PHY teardown, allowing the next OFF→ON session to skip calibration based on the previous PHY's frequency; history is now reset on initialization/deinitialization, with a threshold/reinit regression target authored. | Audit command serialization, reset/re-init, stale transaction, counter wrap, completion/error publication, cancellation, and every I2C/SPI/PIO failure path. Verify stopped-clock snapshot timeout, compare-arm rejection, SPDT mismatch, bool-failure propagation, control-GPIO mismatch, queued-command state change/unknown ID, both AXI/RFIC address boundaries, calibration reinit/threshold, status recovery, and NIOS queue error propagation in deferred regressions, then hardware fault injection after the source gate. |
| P0 | NIOS TX mute and standby lifecycle | `_rfic_initialize()` moved the state to OFF before per-channel dispatch, making its saved-TX-mute restore branch unreachable when returning from STANDBY. The init path now snapshots whether a live PHY is being resumed from STANDBY before publishing fail-closed OFF; cold/recovered init reapplies configured defaults, while warm resume restores each channel's saved mute state and retains the existing RFDC calibration baseline. Added a policy regression source. | Run policy and TX mute hardware round-trip regressions after source audit; inject standby snapshot/mute write failures and prove status remains OFF with a recoverable next ON transition. |
| P0 | NIOS scheduled RX fastlock / retune2 | Active handler scope confirmed; opportunistic-preload false-failure counter fixed. Deferred RX failure event path added in bladeRF `3dbb0bbc`/`84ea1730`/`9cdd25c5`; Python event mapping `84b5280`/`1c8d015`; source harness added. `tamer_schedule()` now requires a sticky FPGA comparator-arm acknowledgement. A further lost-IRQ gap is fixed in source for RX and TX: FPGA latches compare-fired independently of IRQ, and `SCHEDULED` queues recover only from that hardware event; no elapsed-time promotion. Regression source covers no-event hold and lost-IRQ recovery; not run. | Review pending/cancel/recovery ordering, status-bit compatibility, and timestamp-clock stall after arm. Queue harness and `rx_retune_failure_notification_live` must pass after gate opens; qualify stopped clock, late arm, and lost IRQ on exact image. |
| P0 | NIOS scheduled TX fastlock / TX retune gate | TX mute/abort and completion flow reviewed in slices; TX packet-length boundary fixed. FPGA gate ERROR/underflow remain muted until fresh ARM plus COMPLETE, and NIOS ARM waits for mailbox toggle acknowledgement and state; no new fail-open transition found in this controller slice. Found and fixed a monitor gap: deferred TX retune failures could occur after confirmed TX disable, while the monitor only polled the failure counter with TX active. The counter is now monitored independently without probing inactive RFIC state; read failures and counter reset are surfaced, and failure revokes the TX RFIC certificate. Added a dedicated `RUNTIME_TX_RETUNE_FAILURE_TX_DISABLED` live harness case. Recent source pass traced queue reservation before FPGA gate ARM, fail-closed retirement on gate/USB failure, immediate fastlock readback, deferred completion sequence/ring validation, and cancel handling after NIOS acknowledgement. Ambiguous USB enqueue responses remain monitored: a later unpaired success or failure revokes TX validity. No new fail-open confirmed in this slice. | Run the focused disabled-TX failure regression after source gate; finish every queue failure/cancel/ambiguous USB response path, gate CDC/status/reset path, and async TX callback behavior. Run controller and queue tests after gate opens. |
| P0 | AD9361 no-OS lifecycle and RF state | Many lifecycle, calibration, tracking cache, RF-port, FIR, gain, and ambiguous-SPI fixes are recorded in NOW. Reviewed the NIOS platform SPI callbacks and RFIC command boundary: only exact RX0/RX1/TX0/TX1/system channel IDs pass validation; command direction and initialized-state constraints are checked before dispatch, and immediate reads initialize the output to a failure canary. Checked AD9361 operations propagate errors through `CHECK_BOOL`; SPI read/write callbacks convert failed bounded transfers to `-EIO`; retune frequency validity is cleared before RF-port/LO/calibration changes and restored only after success. The reported missing `ad9361_init_gain_tables()` implementation is not reproducible: the pinned no-OS source has its declaration, definition, and initialization call; the similarly named `no-OS_local` patch is historical, not the active source. New host lifecycle defect: errors after `ad9361_init()` had published `board_data->phy` returned immediately, leaving partially configured hardware reported initialized; host init now disconnects RFFE, removes the partial PHY, clears FIR cache validity, and preserves the original status. A new multi-device audit found the AD9361 init parameter globals had their per-device `.extra` pointers overwritten in place; host init now passes a local parameter copy. The no-OS AXI adapter also held its device handle process-wide; it now uses thread-local ownership for synchronous AD9361 initialization and clears the binding on every return. Added a two-thread/two-device regression target (`libbladeRF_test_axi_io_thread_context`), authored but not run under the source gate. A further shared-state pass found no mutable global RFIC state in the active AD9361 driver; its synthesis/gain tables are read-only. A debug-only function-static module-enable timer was a cross-device data race and is now thread-local (`a87d3b81`). A fresh direct-call inventory traced every AD9361 call in `rfic_host.c` and `bladerf2.c`: checked wrappers use the no-OS negative-status contract; raw SPI calls translate failures; host fastlock frequency readback and TX monitor enable propagate failure; temperature status is checked; cache invalidation is void by contract; ignored `ad9361_remove()` results are teardown/rollback cleanup where the first error is preserved. No new ignored-result defect confirmed in this pass. New shared-data-path defect: AD9361 RX/TX sample-rate setters both program RX and TX clock chains, while bandwidth updates also run TX quadrature calibration; generic APIs previously fenced only RX, leaving TX RF certification live through those changes. Sample-rate, rational sample-rate, and bandwidth APIs now take the TX fail-closed fence under `dev->lock` before RFIC mutation and release the RX reservation if that fence rejects. Extended `test_rx_shared_clock_invalidation` to cover both-direction fence ordering and failure paths; test remains unrun under the source gate. NIOS RFFE PIO accesses now fail closed when unavailable and verify output loopback; the separate physical NIOS no-response incident remains open because source-level MMIO guards cannot qualify the exact FPGA/NIOS image or diagnose the reported on-board stall. | Finish the full exported API/call-site inventory: validate inputs before bus access, propagate every IO result, invalidate uncertain caches, guarantee cleanup after possibly-applied writes, and audit every other path that can invoke no-OS AXI accessors. Include RX and TX. Deferred regressions must inject failures at each host init stage and verify OFF/uninitialized state and successful retry; run concurrent-device context isolation after source gate; reconcile every ignored-result search. |
| P0 | libbladeRF RX transition transaction | Source slices cover control-plane events, epoch snapshot, invalidation, and transition certification. A close/wait lifetime defect was confirmed: close could destroy the RX condition/mutex and free board state while transition wait was polling or sleeping for first host data. Transition begin, quick-tune begin, and wait now acquire a board-state call reference under `dev->lock`; pre-close requests cancellation, wakes the waiter, waits for active calls to fail closed and drain, then lets generic close acquire the device lock. Unsupported boards are still rejected before mutex access. Close interruption emits a terminal error before abort cleanup. Added `BLADERF_ERR_ABORTED` and a source regression for wake-and-drain; lifecycle helpers are isolated so the test can link the production close gate without pulling in board control-plane code. | Review all early exits and begin/close interleavings; run the waiter/close regression and transition timeout/error/recovery suite after source gate. Exact-image close during active first-host-data wait remains runtime qualification. Also finish retune/cancel interleavings, module disable/re-enable, channel provenance, and status-monitor interaction. Timeout is failure only. |
| P0 | libbladeRF sync RX parser/admission | Audited the sync META header/sample/partial-read paths: epoch mismatch drops the whole message before copying; timestamp prefixes below the boundary are skipped; discontinuity returns any already-copied contiguous prefix and raises overrun; channel/layout admission is rechecked immediately before the first copy; a concurrent invalidation generation change clears `actual_count` and epoch metadata before return. `sync_rx_epoch_expect_id()` resets continuity across epochs and poisons a message already being consumed. The ring/reorder pass found and fixed a stale ownership bug: completed buffers held in `reorder[]` were still labeled IN_FLIGHT, so epoch discard could rebase onto a buffer whose callback had already run. Discard now retires reorder-held slots before selecting a genuinely submitted transfer; the existing traversal regression now covers both kinds of slot. Traced RX1/RX2 single-lane masks and RX_X2's requirement for both enable bits through transition, sync, async, and monitor admission; unavailable or changed masks revoke the certificate. Regression sources cover those policy branches; unrun under the source-audit gate. A further pass checked timestamp-step wrap semantics, RX_X2 unit conversion at message boundaries, every withheld-data event exit, and ordering of valid-prefix data events versus subsequent discontinuity notification. No new defect confirmed; the row remains open for boundary/fault regressions and complete read-exit inventory. | Finish timestamp wrap/gap arithmetic at limits, event-history loss at sync return boundaries, and every read exit with/without copied data. Then run the focused and full deferred suites and prove no caller-buffer write on rejected reads. |
| P0 | libbladeRF async RX, libusb and CyAPI | Common admission, callback ordering, recycle, length bounds, and terminal-transfer withholding reviewed. Traced both backend completion paths: short/oversized lengths and terminal USB failures append the reason before the event-only callback; epoch, timestamp continuity, channel mask, and first-host-data admission are committed under the RX admission lock before a sample callback; stream state is rechecked after application callback before backend resubmission. TX callbacks recheck the RF certificate immediately before resubmission in both backends; a normal certificate revocation returns `WOULD_BLOCK`, while terminal backend errors are reported through the TX transport-failure hook after stream exit. Found and fixed teardown deadlock: sync worker previously used forced thread cancellation after a 3 s wait, then async deinit could wait forever for `STREAM_DONE/IDLE`; sync and public async deinit now request backend shutdown, libusb drains cancellation completions and CyAPI aborts/wakes its endpoint, and public deinit waits for the outer board stream cleanup call to return before freeing storage. `bladerf_stream()` holds an active-call reference, while deinit blocks later starts and prevents a queued start from reaching the backend. Startup races in both USB backends are closed. Expected backend interruption during requested shutdown is normalized to clean completion only when no independent stream error was recorded. Regression must cover active RX/TX shutdown, blocked transfer, zero/long timeout, start/deinit interleaving, repeated close, board cleanup ordering, and proof backend storage is released only after callback drain. | Complete fault injection for short/oversized/failed/cancelled transfers, teardown/restart races, buffer reorder/recycle, stale callbacks, callback exceptions, and status/event ordering; run backend-aware shutdown lifetime regressions after the source gate. |
| P0 | Python Cython wrapper | Channel/integer/enum validation and event mapping are source-audited in slices. New RX retune diagnostics added; C/Cython enum order statically matched at 50 entries. Audited C history cursor gaps/future cursors and Cython propagation: incomplete history revokes positive validity labels and emits `rf_event_history_incomplete` with a reason (`history_overrun`, `page_capacity`, or `invalid_cursor`); poller exceptions emit a fail-closed notification and retry with bounded backoff. Found and fixed a Python check/return race: an event-poller callback could revoke the certificate after the last generation check but before an RX1/RX2 or RX_X2 block was returned. Scanner now commits the block while holding the same lock used to record/revoke certificates; new transition acceptance clears the previous certificate immediately; RX_X2 validation/provenance/deinterleave are within the serialized commit. Regression source added (`sdr-scanner` `5ab6e60c`), unrun under the source gate. `BLADERF_ERR_ABORTED` is now declared in Cython, exposed as `PYBLADERF_ERR_ABORTED`, and covered by a Python regression source (`python_bladeRF` `1f251fc`), unrun. A new poller lifecycle audit found close/registration could strand a callback with no poller; registration and close are serialized, and close from the poller callback thread now fails explicitly rather than closing the native handle during dispatch (`python_bladeRF` `24aeba6`; regression source unrun). Full Cython mutator/stream-control sweep is now complete: every RF-affecting setter, trigger action, schedule/cancel, RX/TX stream operation, and FPGA load dispatches durable native events before returning or raising; the five methods without event dispatch are read-only trigger state/init, host timeout policy, clock-output GPIO, and global USB reset policy. | Finish callback concurrency and exception/teardown interleavings, local-extension/local-library provenance, and verify channel-level semantics for RX1/RX2/RX_X2. Rebuild from this checkout after the source gate. |
| P1 | RF content, DC/IQ correction, spurs/shielding | Active correction path and no-OS tracking state reviewed; no confirmed new xA4 correction defect in reviewed slices. | Finish writer/startup/register inventory. Then compare repeatable conducted IQ captures/FFT with identical settings; do not change correction or spur masks without RF evidence. |
| P1 | Host/package/build provenance | Python release linker rejects RPATH/RUNPATH; local build script is designed to force sibling wrapper/library. | Verify build script inputs, generated Cython source policy, extension/library hashes, installed module path, ABI/version, and release archive manifest. |

### Newly confirmed native public-path finding

Arbitrary public Wishbone and config-GPIO writes revoked RX validity but left
the TX RF certificate active, despite the writes being able to alter FPGA
admission, clock, or RF controls. The generic mutators now acquire the TX
configuration fence before invoking board writes; the xA4 board callbacks no
longer acquire it a second time. The same ownership correction was applied to
sample-rate, bandwidth, band/port, loopback, and trim-DAC setters after tracing
the previous generic fix into xA4 callbacks and finding duplicate TX
invalidation events on the real path. The focused regression source checks the
single-fence ordering for raw writes and TX band/port setters, and fail-closed
behavior. Source changes are committed as bladeRF `544521b3`, `90e2626e`, and
`216a40a9`; the test remains unrun under the source-audit gate.

The adjacent board-specific RFIC mutators were traced separately: AD9361 RX
and TX FIR setters fence the opposite active direction where shared datapath
state can change; raw RFIC and ADF400x register writes, PLL/reference-clock
changes, clock-source selection, and bias-tee writes have their own RX/TX
reservation paths. This review slice did not confirm another missing-fence
case. The exported API inventory and failure interleavings remain open.

RX module enable/disable revokes the active RX certificate before channel-mask
changes. TX enable while RX is live explicitly invalidates RX before the
shared START; TX disable leaves an active RX link running. The reservation is
released by the generic completion callback on both success and failure.
Source review found no missing invalidation in this module lifecycle slice;
RX1/RX2/RX_X2 restart races and hardware fault injection remain open.

The NIOS scheduled-retune queue/response path was also traced end to end.
USB transport timeout marks the peripheral channel desynchronized and does not
replay the request. The enqueue response is treated as acceptance only;
deferred execution failures are counted separately, successful completions
publish sequence-tagged records, and host completion reads verify the sequence
before and after the record fields before matching against a pending request.
No additional fail-open was confirmed in this source slice. Queue overflow,
ambiguous USB response, cancellation, counter wrap/reset, and the physical
NIOS no-response incident remain test/hardware gates.

The integrated RX gate/controller/FIFO boundary was re-read in RTL. ARM,
COMPLETE, and ABORT are decoded from one bundled control word; gate outputs
register IQ and timestamp in the same sample-domain stage; RX_X2 first-valid
requires valid samples on every enabled lane; only then is the new epoch's
first timestamp latched and the sample admitted. ABORT wins over ARM/COMPLETE
and fenced states preserve transport using zero-IQ keepalives. Sample-META
legacy layout remains separate when epoch tagging is disabled. This source
slice found no additional stale-IQ admission path. Clock/reset recovery,
packet-straddling timestamp traces, RX1/RX2/RX_X2 simulation, and exact-image
hardware qualification remain open.

### RX link-status continuity finding

The background RX monitor and transition admission now require protocol-v2
`RX_EPOCH_CURRENT`, `RX_EPOCH_VALID`, and `RX_LINK_ACTIVE` in addition to a
clear sticky RX fault. Before this guard, a reset or lost START acknowledgement
could leave host admission certified, or permit transition certification,
without a confirmed FPGA RX link epoch. The monitor revokes admission and
publishes an FPGA RX invalidation; the transition fails before host admission.
Source regressions cover each missing status bit. Runtime fault injection now
covers both loss during a certified stream (`RUNTIME_FPGA_RX_EPOCH_LOST`) and
loss at the final pre-certification status observation (`FPGA_EPOCH_LOST`),
requiring no `RX_EPOCH_VALID` event and no IQ delivery. bladeRF commits
`75bb2d63`, `e3a7adbe`, and `28710989`.
Regressions and hardware qualification remain unrun under the open source-audit
gate.

### Event-ring page capacity versus actual history overrun

The native cursor API distinguishes a bounded-ring overrun (`status == 0`,
`history_complete == false`) from a caller buffer that is too small
(`BLADERF_ERR_MEM`, partial page, cursor advanced only through copied events).
The Python wrapper previously labeled both cases `rf_event_history_lost`,
which falsely claimed that retained events had been overwritten. It now emits
`rf_event_history_incomplete` with `incomplete_reason` set to
`history_overrun`, `page_capacity`, or `invalid_cursor`; all remain fail-closed
for RX/TX validity, and the next poll continues from the returned cursor.
Scanner consumes the new event name while retaining compatibility with older
wrappers. Added regression source only; tests remain deferred until the source
audit gate closes.

### RX PACKET_META length bounds

The sync RX packet path trusted the payload DWORD count from the packet header
when copying into the caller's buffer. A corrupt/short packet could make the
parser read beyond received bytes, and a valid packet larger than `num_samples`
could overrun the application destination. Async RX now rejects headers whose
declared payload exceeds the actual transfer or allocated buffer; sync RX
rejects packets exceeding caller capacity before copying. Added source
regressions for truncated/oversized transfer lengths and output capacity. No
test/build run under the open source-audit gate.

### Mutex initialization failure propagation

The complete libbladeRF `MUTEX_INIT` call-site inventory contained ignored
results in device open, USB backend open, bladeRF2 board open, RX fault-monitor
startup, sync/async stream setup, and sync-worker setup. Those paths could mark
failed mutexes initialized and then lock or destroy invalid objects during
normal work/cleanup. `MUTEX_INIT` now returns a status on POSIX and Windows;
each caller checks it and unwinds only successfully initialized resources.
This slice has source review and diff checks only; runtime fault injection and
all builds remain deferred.

### NIOS scheduled-retune cancel/ISR source pass

The retune2 queue is mutated by the NIOS main-loop packet/work handlers; the
timer ISR only changes the head state from `SCHEDULED` to `READY`. Queue clear
sets the count to zero before invalidating entries, so an ISR during clear sees
an empty queue. The host queue is reset only after the timestamp-domain cancel
acknowledgement toggles; an acknowledgement timeout returns an error and does
not claim that cancellation succeeded. A successful ordinary RX scheduled
retune still leaves RX validity revoked until a later explicit epoch
transition; queue acceptance is not IQ-valid evidence. No new source defect
confirmed in this slice. ISR/cancel race injection, comparator stopped-clock,
and exact-image retune timing remain open.

### NIOS scheduled-retune lost-IRQ source finding

Both retune queue implementations previously depended exclusively on the
time-tamer IRQ after comparator arm. If that IRQ was lost, the head remained
`SCHEDULED` indefinitely and retune2 never published its per-direction
failure event. The FPGA now latches comparator completion in a sticky status
bit independently of IRQ delivery. RX and TX workers can advance from that
hardware event; time passing alone cannot mark a queue ready. The source
harness adds lost-IRQ recovery coverage. RTL/status compatibility,
timestamp-clock stall after successful arm, and exact-image qualification
remain open. No build, simulation, or test was run under the source-audit gate.

### AD9361 AXI converter call-result inventory source pass

Reviewed the active AD9361 converter/init/IIO slices that call AXI ADC/DAC
operations, together with the NIOS sample-rate mutation path. Converter
register reads/writes and status-returning helpers propagate errors; post-setup
uses a fail-fast wrapper; digital-tune cleanup preserves the original error
and promotes a cleanup failure when the main operation succeeded. The AXI
ADC/DAC remove functions used during rollback return unconditional success
after freeing their descriptors, so ignored cleanup return values do not mask
a reported failure in this implementation. NIOS sample-rate state is marked
invalid before mutation and restored only after the full RX retune sequence
succeeds. No new defect was confirmed in these slices. The complete exported
AD9361/no-OS and NIOS call-site inventory, fault injection, and runtime gates
remain open; no build or test was run.

The wider NIOS peripheral wait pass found a resource leak in the bounded SPI
arbiter acquire path: timeout returned while leaving the NIOS request asserted.
The arbiter could later grant that orphan request and remain owned after the
failed caller had exited. Timeout now clears the request and acknowledges
release through the same policy helper used by a focused delayed-grant/timeout
regression source. The suite is authored but not run under the source gate.

The same pass found legacy LMS SPI read/write helpers hid arbiter acquisition
failure as zero data or a no-op, and 8x8/legacy packet handlers marked those
operations successful. The helpers now return explicit status, which is
propagated into both packet formats and the no-OS compatibility callbacks;
the existing peripheral packet regression source now injects LMS read and
write failures. bladeRF1 legacy path only; no hardware/runtime claim.

The Wishbone extension bridge also had an unbounded external-ACK wait: a
missing ACK kept the Avalon access pending and could stop NIOS before it sent
the packet response. Added a Wishbone-clock watchdog, timeout-tagged response,
system-clock sticky status/clear, and NIOS status propagation. After timeout
the bridge locks out external Wishbone requests until reset and fails queued
requests, preventing a late ACK from validating a later transaction. Added a
testbench case for no ACK and post-timeout lockout, plus NIOS packet read/write
failure regression source. The xA4 top-level's built-in termination target
still ACKs immediately; its no-response incident is not attributed to this
path. HDL simulation and exact-image qualification remain deferred.

### NIOS no-OS AXI timeout and init compile-path pass

The NIOS no-OS adapter forwards AXI ADC/DAC reads and writes through
`adi_axi_read/write()`, whose boolean result is converted to `-EIO`; converter
init checks each register access. The AXI slave returns a bounded response
after its 31-cycle counter, and the sticky timeout PIO is cleared/read in the
same `system_clock` domain as `up_axi`, so the suspected timeout CDC gap is
not present. A separate compile-path inconsistency was confirmed:
`BLADERF_NIOS_BUILD` excludes `axi_adc_init()` and `ad9361_post_setup()` in
`ad9361_init()`, leaving the NIOS-owned PHY without `rx_adc`, but the NIOS
firmware sample-rate handler still calls `ad9361_dig_tune()`, whose converter
validation rejects the missing handle with `-EINVAL` before issuing AXI
traffic. The CMake ablation only stubs converter/tuning helpers and cannot
establish that `ad9361_init()` AXI access caused a hardware wedge. Corrected
those stale comments. The NIOS sample-rate contract, active compile
command/image, and physical no-response incident remain open; no behavioral
change or hardware conclusion is claimed.

### Sync RX concurrent-invalidation output-buffer finding

The final sync RX generation fence correctly returned `WOULD_BLOCK` and
cleared metadata if RF invalidation raced after the parser's last in-loop
check. However, if the parser had already copied old-epoch samples into the
caller buffer, that error path left the bytes present while reporting
`actual_count == 0`. The final fence now tracks bytes written across packed,
sample-META, and packet-META paths and clears those bytes when generation
changed before return. Added a deterministic regression that blocks immediately
before META copy, revokes the generation, resumes the copy, and requires the
read to fail with zeroed output and no first-host-data event. Source only;
regression remains unrun under the gate.

### Async RX backend/admission source pass

Re-traced libusb and CyAPI callback exits. Terminal transport errors publish
the reason-coded withheld event and event-only callback before stream teardown;
requested shutdown is handled separately and does not become a transport
fault. Completed transfers pass through the shared length/epoch/timestamp/
channel validator, which commits the timestamp cursor and first-host-data
event under the RX-state lock before the sample callback. The callback path
rechecks stream state before resubmission. In sync mode, the reject hook
retires the submitted sequence and chooses a replacement ring slot before
the withheld callback can wake the reader. No new source defect confirmed in
these slices. Backend fault injection, stale callback and close/restart races,
and exact-image RX1/RX2 qualification remain open; no build/test was run.

### Sync RX delivery/invalidation linearization slice

`sync_rx()` snapshots the RX generation before locking the parser, checks it
while consuming buffers, validates the native certificate immediately before
copying META IQ, then rechecks the generation at the return boundary while
holding the generation lock through sync-lock release. If invalidation won the
race, it clears `actual_count` and epoch metadata and reports a withheld-data
reason; copied bytes are not reported as successful samples. Python drains RF
events after the native call; if an otherwise successful read cannot complete
event dispatch, it zeros the output buffer and raises. No additional source
defect confirmed in this slice. Read-path review now covers the worker state
machine, stale/rejected ring slots, sample-only copies, META header/epoch
dispositions, partial message copies, PACKET_META capacity rejection, and the
common return path. Transitions that require `RX_EPOCH_VALID` reject active
sync formats without epoch tags; stream replacement also refuses an epochless
format while preserving an existing epoch contract. `PACKET_META` does not
pretend its packet header is an epoch tag. The remaining contract is explicit:
legacy/non-epoch reads remain available when the caller did not request epoch
validity, while stream loss and malformed packets still publish native fault
events.

The source audit did not run the promised caller-buffer sentinel checks. Keep
open: boundary/fault regressions proving no caller-buffer write on rejected
reads, event-history loss interleavings at every return branch, and deferred
regression execution. In particular, verify that each nonzero return's
`actual_count` semantics agree with the public API for partial META reads and
PACKET_META errors; no implementation change is made without that evidence.

### Sync TX timeout accounting

The neighboring TX pass found that `sync_tx()` passed the full caller timeout
to every buffer wait, so a multi-buffer call could take several multiples of
the documented call timeout. RX also measured its newly added overall deadline
with `CLOCK_REALTIME`, which can jump when wall time is adjusted. Both paths
now share a monotonic remaining-time calculation, bound worker startup and
buffer waits by the remaining caller budget, and check the RX deadline while
consuming ready buffers. TX buffer submission and deferred-buffer reclaim now
use the remaining caller budget for the backend's stream-start wait. A 1 ms
handoff allowance at an already-expired boundary lets the normal ownership
transfer/rollback path finish instead of stranding a full ring slot; timeout
zero preserves an unbounded wait. Focused timeout, long-copy, timeout-zero,
clock-adjustment, and partial TX buffer recovery regressions remain deferred
under the source-audit gate. Source review found a cross-platform failure in
the timeout implementation: the Windows `clock_gettime` shim rejected
`CLOCK_MONOTONIC`, and the old macOS shim returned calendar time for every
clock ID. The shims now provide monotonic time, POSIX condition variables use
monotonic-clock deadlines (macOS uses its relative wait API), and worker/async
startup loops recalculate the remaining budget after every wake. This also
closes timeout extension after spurious or unrelated signals. Existing
libbladeRF monotonic timestamp call sites now use the same platform helper so
they do not depend on undeclared POSIX `clock_gettime()` symbols on Windows.
Windows/macOS build and runtime checks, repeated-wakeup tests,
clock-adjustment tests, and partial TX buffer recovery remain deferred under
the source-audit gate.

The timestamp seek boundary pass found an integer overflow in sync RX: a very
large future timestamp multiplied by the RX_X2 lane factor before comparing
against the remaining buffer length. It could mis-seek or wait until the read
deadline. The seek now uses the bounded timestamp-to-samples helper, which
clamps the requested gap to the available remainder before multiplication;
regression source exercises huge deltas with even and odd available lengths.
bladeRF `d3fe0489`. No tests/build run under the open source-audit gate.

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
   deferred RX/TX retune notification paths.
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

### Legacy NIOS packet malformed-request fail-open

A source pass found that the legacy NIOS dispatcher preserved the success
magic for an unsupported device ID and for packets with no valid direction
bit. Legacy config reads also indexed `CONFIG_UNKNOWN` and returned a normal
response for an unsupported config address; writes relied on the zero-length
sentinel and could leave multi-packet aggregation state inconsistent. The
legacy firmware now returns the protocol error magic for those cases and
resets the affected config aggregation state. Simultaneous READ and WRITE
flags are rejected. This is source-only; packet-level firmware regression and
full release audit remain open.

### NIOS 8x32 diagnostic subaddress status

The xA4 8x32 handler previously returned packet success with data zero for
out-of-range retune failure/success counters, completion-record fields,
loss-counter halves, and dwell-record words. A caller typo or ABI mismatch
could therefore be mistaken for a valid zero diagnostic. These cases now
preserve zero data but return packet failure; valid address ranges remain
unchanged. A shared bounds helper and regression source now cover the last
valid and first invalid subaddress for each diagnostic family. The generic packet runner
compiles without the xA4 board macro, so it does not exercise the guarded
packet response branch; add a board-path packet regression before the
source-audit gate closes. No build or test run.

A direct source review of `rx_fault_monitor_task()` confirmed its certified-RX
failure handling order: channel-mask read failure/change, FPGA status transport
failure or loss of current/valid/active bits, loss-counter read/reset, RFIC
status read failure, PLL unlock, ENSM departure, BBPLL unlock, and sticky RX
fault each enter the epoch-scoped invalidation path. That path revokes sync and
async admission and durably appends `RX_DATA_INVALIDATED` before the monitor
attempts FPGA ABORT; abort failure and the stream-overrun notification are
reported separately. This source slice found no monitor fail-open. Runtime
fault injection and RX1/RX2 exact-image qualification remain open.
