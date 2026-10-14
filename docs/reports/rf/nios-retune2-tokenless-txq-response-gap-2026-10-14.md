# Tokenless RETUNE2 TX calibration can outlive the host response deadline

Date: 2026-10-14

## Finding

The RETUNE2 `NOW` command has two execution paths in the xA4 NIOS source. A request with a nonzero request token is enqueued and completed by the cooperative retune worker. A tokenless request preserves the legacy synchronous response contract and calls `profile_activate()` directly.

For TX, `profile_activate()` performs fastlock activation and then `rfic_read_frequency_live()`. When TX quadrature calibration is required, that function calls `rfic_finalize_tx_quad_calibration()` on the tokenless path. The latter calls `ad9361_ensure_tx_quad_calib()`, whose synchronous loop repeatedly polls the retained ADI calibration transaction and calls `no_os_udelay()` until it reaches a terminal result. The queued path instead starts and polls that same calibration through worker turns.

The host sends RETUNE2 through `nios_access()`, which uses `PERIPHERAL_TIMEOUT_MS` (250 ms) as the NIOS transaction response timeout. `nios_transaction.c` preserves a timed-out RETUNE2 response for later recovery only when the request has a nonzero token. A tokenless RETUNE2 response timeout falls through to the generic ambiguous/partial-response handling and can mark the request channel desynchronized. Thus a supported legacy host/FPGA combination can wait synchronously for TXQ calibration longer than the host response deadline and lose the response contract while NIOS is still doing calibration work.

This is a concrete source-level liveness and response-correlation gap. It is not evidence that this path caused the preserved hardware no-response incident. The current host allocates request tokens when the FPGA advertises `BLADERF_CAP_RETUNE2_REQUEST_TOKEN`; the tokenless path remains relevant to older host libraries and images without that capability.

## Source correction now implemented (unverified)

Tokenless TX `NOW` now uses the same retained queue as tokened `NOW`. The queue entry retains the original packet buffer, and TXQ success/failure prepares the legacy response only after the worker reaches a terminal result. The NIOS main loop keeps the request ISR fenced while that response is pending and routes the completed packet through the existing immutable-response retry path. This prevents early success and prevents request-side effects from being replayed when UART publication is backpressured.

The host now uses a scoped 120 s IN-response watchdog only for tokenless TX `NOW`; tokenless RX `NOW` has a separate 5 s watchdog (see follow-up below), and ordinary NIOS requests retain the 250 ms deadline. The TX watchdog matches the existing tokened terminal-result watchdog and is much longer than the source-derived TXQ calibration cycle budget (35 attempts, approximately 3.22 s at the locally derived minimum CLKRF). These are transport failure watchdogs, not calibration-completion predicates. Whole-command setup/restore, SPI transfer, and scheduler-service upper bounds still need source closure; revise the watchdogs from those bounds if they are invalidated. A zero-byte timeout still fails closed and desynchronizes the host channel; it never reports RF success.

## Follow-up finding: tokenless RX `NOW` also bypassed the worker

The same branch audit found that tokenless RX `NOW` synchronously called `profile_load()` and `profile_activate()` from packet dispatch. `adi_fastlock_recall()` then busy-polled the RX RFPLL for up to 20 ms, while the token-capable path already split profile upload and lock/readback across worker turns. This left the main RX sweep path with a separate blocking island even though it has no TXQ calibration.

The source now routes every tokenless `NOW` direction through the retained queue, retaining the original packet buffer until a terminal success/failure response is ready. Tokened callers retain their acceptance/completion protocol. For host response watchdogs, untagged TX remains at 120 s because of the documented TXQ search; untagged RX uses 5 s. The RX value is a failure watchdog, not a validity rule: source adapter limits give 18 profile-upload writes × (10 ms arbiter acquire + 10 ms SPI transfer + 10 ms release) = 540 ms before PLL, RF-port, readback and USB work. This is a conservative adapter-only estimate, not a measured or complete whole-command WCET; the aggregate command/service-gap bound remains open.

Regression source now covers tokenless RX deferred success and PLL failure as well as TXQ terminal success/failure, and host tests assert both directions select an extended timeout. These tests have not been built or run.

## Source anchors

- `hdl/fpga/platforms/common/bladerf/software/bladeRF_nios/src/pkt_retune2.c`: current source enqueues all tokenless `NOW` requests; older source called `profile_activate()` with `allow_txq_yield=false`.
- `hdl/fpga/platforms/common/bladerf/software/bladeRF_nios/src/devices_rfic.c`: TX fastlock readback finalizes TXQ synchronously when yielding is disabled; the queue path starts/polls the retained transaction.
- `thirdparty/analogdevicesinc/no-OS/drivers/rf-transceiver/ad9361/ad9361.c`: `ad9361_ensure_tx_quad_calib()` polls to terminal in a loop; the transaction start/poll API is available for cooperative continuation.
- `host/libraries/libbladeRF/src/backend/usb/nios_access.c`: NIOS packet transaction uses the 250 ms peripheral response timeout.
- `host/libraries/libbladeRF/src/backend/usb/nios_transaction.c`: timeout recovery retains RETUNE2 only for nonzero request tokens.

## Regression source added; verification pending

- Tokenless TX `NOW` with TXQ required returns no final response before terminal completion, then exactly one correct success/failure response. Added to `host/misc/test_nios_retune2_queue.c`.
- Repeated worker turns advance TXQ without blocking other NIOS status/progress servicing.
- Backpressure after terminal completion retries the prepared immutable response without rerunning fastlock or calibration.
- Session reset/reopen retires the old response and cannot associate it with a later request.
- Host timeout/retry and late response preserve request identity and never report an unknown result as success.
- No-capability legacy host/image and token-capable current host/image both follow explicit, tested contracts.
- Host-side regression asserts tokenless RX/TX `NOW` get their scoped extended IN timeout (`host/misc/test_nios_rfpll_batch_host.c`).

## Verification status

Source change and regression source only; `git diff --check` is the only verification so far. No build, test, simulation, synthesis, image load, or hardware qualification was run. Review the full deferred-response/session lifecycle and the watchdog bound with the surrounding NIOS dispatcher before the single planned verification/image cycle.

ADI's UG-570 Eq. 11 defines maximum TX quadrature calibration duration as `TX channels × 94,464 CLKRF cycles`: [AD9361 Reference Manual UG-570](https://www.analog.com/media/en/technical-documentation/user-guides/AD9361_Reference_Manual_UG-570.pdf). The 3.22 s aggregate is the local xA4 cycle-budget calculation for the current 35-start source path; it is not an observed runtime measurement.
