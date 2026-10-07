# RX transition deadline is enforced after hardware observations

The event-driven wait previously checked `timeout_ms` before each PLL,
ENSM, BBPLL, or FPGA epoch status read. Those reads can block on SPI/USB.
If one began before the deadline and returned after it with the requested bit
set, the wait could accept a late observation and publish a successful
transition.

The wait now checks the monotonic deadline immediately after every required
hardware observation and before installing the host timestamp fence or
publishing `RX_EPOCH_VALID`. Late PLL/ENSM/BBPLL, FPGA COMPLETE/status,
timestamp, link-status, or host-fence completion returns
`BLADERF_ERR_TIMEOUT`, publishes terminal `ERROR`, and aborts the epoch gate.
The public header documents that an observation at or after the deadline does
not satisfy the wait. No sleep or sample discard establishes validity.

The host timestamp fence is now two-phase. libbladeRF stages the exact
timestamp and epoch while leaving `rx_epoch_data_invalidated` set, publishes
`RX_EPOCH_VALID`, then activates sync RX. Activation checks the same absolute
deadline under the parser mutex; a timeout while waiting on that lock leaves
IQ fenced. This also orders the first host-data lifecycle event after the
durable epoch-valid event. Async-only RX has no sync parser; it checks the
deadline before event publication, which commits async admission.

The test-only transition fault build can delay a selected successful
observation by 150 ms. The positive-timeout harness now covers ordinary
missing-state stalls and late PLL, ENSM, BBPLL, COMPLETE, epoch, timestamp,
link-status, host-fence, and post-event host-activation observations on RX1,
RX2, and paired RX_X2. The
deadline comparator also has exact-before/equal/after boundary assertions.

Validation completed:

- production `libbladerf_shared` build passed;
- test-injection library and strict C harness compilation passed;
- RX transition requirement policy and RX epoch metadata tests passed;
- sync epoch traversal and shared-clock invalidation tests passed;
- sync traversal verifies that a past-deadline fence leaves the parser
  invalidated and withholds a matching epoch/timestamp packet;
- sync traversal verifies staging does not release data and that activation
  after the deadline leaves the parser invalidated;
- a lock-contention regression starts the fence before deadline, holds the
  parser mutex past it, and verifies the fence returns timeout without
  admitting IQ;
- `hdl/quartus/qcheck` passed after updating its stale source token from the
  removed metadata-preflight helper to the current sync invalidation path;
- `git diff --check` passed.

The live `RX1/RX2/RX_X2` harness could not open the attached device:
`get_fpga_version` returned `Operation timed out`. Therefore late-observation
hardware behavior remains unqualified; the failure is not counted as a test
pass. The test-only library and strict C harness compile were repeated after
the deadline-aware sync-fence change. No services were started.
