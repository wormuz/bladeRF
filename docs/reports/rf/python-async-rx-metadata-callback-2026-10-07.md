# Python async RX metadata callback

Date: 2026-10-07

## Finding

The Python Cython async RX callbacks received native `bladerf_metadata *meta`
but discarded it. Applications therefore received IQ arrays without the
timestamp, RX epoch ID, stream layout, or native metadata status associated
with the array. This prevented an async Python consumer from correlating IQ
with the validity/provenance contract exposed by libbladeRF.

## Change

Added `PyBladerfDevice.set_rx_callback_with_metadata()`, an additive API that
calls the registered callback with the existing four arguments plus a copied
metadata dictionary. The snapshot includes timestamp, flags, status,
`actual_count`, optional `rx_epoch_id`, epoch-ID validity, layout, and sample
count. The legacy `set_rx_callback()` keeps its four-argument signature and
clears the metadata-aware callback when selected.

The callback does not synthesize `iq_valid` from a nonzero sample count:
native async RX accepts untagged buffers when the epoch contract is disabled,
so sample count alone cannot certify validity. The callback exposes provenance
only; callers must use the transition/event validity contract.

The native async RX path now copies the validated first META timestamp and
certified epoch ID into callback-scoped `bladerf_metadata` only after the whole
transfer passes epoch/timestamp admission. The async adapter clears these
fields before each transfer, so event-only withholding callbacks cannot inherit
identity from the prior block. Legacy/unfiltered callbacks retain no epoch
claim.

The snapshot is Python-owned; it does not retain a pointer to the native
callback metadata. RX_X2 layout is included so consumers can preserve paired
lane provenance. Event-only zero-sample wakes continue through the existing RF
event notification path and do not invoke the IQ callback.

## Validation

- Cython extension built against the active bladeRF checkout headers/library.
- Native sync/async traversal regression and transition policy pass.
- Python wrapper suite: 27 passed against the rebuilt libbladeRF.
- Worker-stop regression and HDL `qcheck` pass.
- `git diff --check`: passed.
- No live hardware callback qualification was performed for this change.

The callback exposes metadata supplied by native libbladeRF; it does not
independently certify RF analog settling or decoder success.
