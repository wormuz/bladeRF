# NIOS RFIC USB request is no longer replayed after response timeout

Date: 2026-10-09

## Finding

The previous `nios_rfic_access_retry()` loop reran the complete USB exchange
after a response (`EP_IN`) timeout. Each retry therefore sent another request
on `EP_OUT`. The 16-byte command UART protocol carries no transaction ID, and
the FPGA/NIOS request path stores a single request in `pkt.req` with a boolean
`pkt.ready`; it cannot distinguish a retry from a second RFIC command or pair
a delayed response with its original request. A repeated RFIC write can
therefore duplicate a side effect, and a repeated request can overwrite a
pending request. This is a protocol-level correctness risk; the preserved
55-second nonresponse trace does not prove it caused that incident.

## Change

RFIC access now sends one OUT request and performs one bounded IN transfer
with a 6000 ms response deadline. The USB peripheral mutex remains held across
the pair. The request is never replayed after an ambiguous response timeout.
All other NIOS packet traffic retains the regular 250 ms transfer timeout.

Both packet formats now use one shared internal transport helper for the
atomic OUT/IN pair. It reports whether the failure happened before OUT, during
IN, or because the channel was already desynchronized. This keeps legacy and
modern callers on the same fail-closed rule.

If any NIOS OUT or IN transfer fails, the USB handle's peripheral channel is
marked desynchronized. Subsequent legacy and modern NIOS packets fail before
OUT is sent; the flag is cleared only by constructing a new USB handle during
close/open. This prevents a late response from being mistaken for a later
request. It does not recover a stalled NIOS or prove RFIC state after timeout.

## Verification

- `libbladerf_shared` rebuilt successfully with warnings treated as errors.
- `libbladeRF_test_nios_transaction` passed fake-backend checks for successful
  pairing, a single 6000 ms IN timeout, sticky desynchronization, and no
  follow-up USB transfer after an ambiguous failure.
- `libbladeRF_test_sync_epoch_traversal` rebuilt and passed.
- `qcheck` remains clean.
- No hardware command or RF test was run; the preserved NIOS failure state
  was not disturbed.

The current function has not yet been exercised against a live FPGA/FX3 after
this transport change. Exact response latency and the original NIOS
no-response cause remain open qualification items.
