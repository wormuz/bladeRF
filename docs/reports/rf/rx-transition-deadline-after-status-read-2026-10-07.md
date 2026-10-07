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

The host timestamp fence receives the same absolute monotonic deadline. For
sync RX, it checks the deadline while holding the parser mutex and leaves
`rx_epoch_data_invalidated` set if the deadline expired while waiting for
that lock. This prevents a late fence update from briefly releasing queued IQ
before the transition returns timeout. Async-only RX performs the same
deadline check before its epoch-valid notification.

The test-only transition fault build can delay a selected successful
observation by 150 ms. The positive-timeout harness now covers ordinary
missing-state stalls and late PLL, ENSM, BBPLL, COMPLETE, epoch, timestamp,
link-status, and host-fence observations on RX1, RX2, and paired RX_X2. The
deadline comparator also has exact-before/equal/after boundary assertions.

Validation completed:

- production `libbladerf_shared` build passed;
- test-injection library and strict C harness compilation passed;
- RX transition requirement policy and RX epoch metadata tests passed;
- sync epoch traversal and shared-clock invalidation tests passed;
- sync traversal verifies that a past-deadline fence leaves the parser
  invalidated and withholds a matching epoch/timestamp packet;
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
