# Tokenless RETUNE2 TX calibration can outlive the host response deadline

Date: 2026-10-14

## Finding

The RETUNE2 `NOW` command has two execution paths in the xA4 NIOS source. A request with a nonzero request token is enqueued and completed by the cooperative retune worker. A tokenless request preserves the legacy synchronous response contract and calls `profile_activate()` directly.

For TX, `profile_activate()` performs fastlock activation and then `rfic_read_frequency_live()`. When TX quadrature calibration is required, that function calls `rfic_finalize_tx_quad_calibration()` on the tokenless path. The latter calls `ad9361_ensure_tx_quad_calib()`, whose synchronous loop repeatedly polls the retained ADI calibration transaction and calls `no_os_udelay()` until it reaches a terminal result. The queued path instead starts and polls that same calibration through worker turns.

The host sends RETUNE2 through `nios_access()`, which uses `PERIPHERAL_TIMEOUT_MS` (250 ms) as the NIOS transaction response timeout. `nios_transaction.c` preserves a timed-out RETUNE2 response for later recovery only when the request has a nonzero token. A tokenless RETUNE2 response timeout falls through to the generic ambiguous/partial-response handling and can mark the request channel desynchronized. Thus a supported legacy host/FPGA combination can wait synchronously for TXQ calibration longer than the host response deadline and lose the response contract while NIOS is still doing calibration work.

This is a concrete source-level liveness and response-correlation gap. It is not evidence that this path caused the preserved hardware no-response incident. The current host allocates request tokens when the FPGA advertises `BLADERF_CAP_RETUNE2_REQUEST_TOKEN`; the tokenless path remains relevant to older host libraries and images without that capability.

## Required correction

Make tokenless TX `NOW` use the same retained TXQ worker instead of calling the blocking ADI wrapper. Preserve its synchronous API meaning by deferring the packet's final success/failure response until the worker reaches a terminal result; never return early success and never replay the RF operation to recover a response. Keep request/session identity attached to the deferred response, retire stale-session responses, and prove that packet dispatch cannot execute the original request again while its final response is pending.

The host response watchdog must then be derived from the bounded worker operation and transport behavior, or the compatibility path must use a correlated completion protocol. Do not merely raise the global 250 ms timeout: USB OUT and IN stages have distinct needs, and late tokenless response recovery currently has no request identity. Establish a source-grounded worst-case TXQ completion bound, including ADI attempt/phase limits, ENSM transitions, SPI transfer bounds, cleanup, scheduler service gaps, and transport deadlines before selecting a watchdog.

## Source anchors

- `hdl/fpga/platforms/common/bladerf/software/bladeRF_nios/src/pkt_retune2.c`: tokened `NOW` enqueues; tokenless `NOW` calls `profile_activate()` with `allow_txq_yield=false`.
- `hdl/fpga/platforms/common/bladerf/software/bladeRF_nios/src/devices_rfic.c`: TX fastlock readback finalizes TXQ synchronously when yielding is disabled; the queue path starts/polls the retained transaction.
- `thirdparty/analogdevicesinc/no-OS/drivers/rf-transceiver/ad9361/ad9361.c`: `ad9361_ensure_tx_quad_calib()` polls to terminal in a loop; the transaction start/poll API is available for cooperative continuation.
- `host/libraries/libbladeRF/src/backend/usb/nios_access.c`: NIOS packet transaction uses the 250 ms peripheral response timeout.
- `host/libraries/libbladeRF/src/backend/usb/nios_transaction.c`: timeout recovery retains RETUNE2 only for nonzero request tokens.

## Required source regressions

- Tokenless TX `NOW` with TXQ required returns no final response before terminal completion, then exactly one correct success/failure response.
- Repeated worker turns advance TXQ without blocking other NIOS status/progress servicing.
- Backpressure after terminal completion retries the prepared immutable response without rerunning fastlock or calibration.
- Session reset/reopen retires the old response and cannot associate it with a later request.
- Host timeout/retry and late response preserve request identity and never report an unknown result as success.
- No-capability legacy host/image and token-capable current host/image both follow explicit, tested contracts.

## Verification status

Source inspection only. No build, test, simulation, synthesis, image load, or hardware qualification was run. The grouped source audit remains open; this defect must be fixed and covered before the single planned verification/image cycle.
