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
- `hdl/quartus/qcheck` passed after updating its stale source token from the
  removed metadata-preflight helper to the current sync invalidation path;
- `git diff --check` passed.

The live `RX1/RX2/RX_X2` harness could not open the attached device:
`get_fpga_version` returned `Operation timed out`. Therefore late-observation
hardware behavior remains unqualified; the failure is not counted as a test
pass. No services were started.
