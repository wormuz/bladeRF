# Sync RX first-host-data deadline is enforced before IQ copy

The sync META parser could publish `RX_FIRST_VALID_HOST_DATA` after reading a
buffer without enforcing the transition's required first-host-data deadline
before copying its first samples to application memory. Async META admission
already applied the timeout contract. A late sync packet could therefore
escape the same deadline that was meant to terminate the transition.

Sync META admission now calls a board policy callback immediately before the
payload copy. The bladeRF 2 policy checks the certified epoch and first-valid
timestamp, selected RX channel mask, and required first-host-data deadline,
and captures the admission monotonic timestamp. A late first packet is
withheld and returns `BLADERF_ERR_TIMEOUT`. Host-data history is committed
only after the sync call has copied samples and passed its final epoch
generation check; it carries the earlier admission timestamp, so a copy that
finishes just after the deadline is judged by when admission occurred. The
lower-level host event publisher also checks that timestamp so late data
cannot be certified.

The admission policy is channel-mask aware. RX1-only and RX2-only X1 streams
must match their selected lane; RX_X2 requires the paired mask. This change
does not claim new dual-RX qualification: it closes the same validity gap for
all layouts using the shared policy. The xA4 live check remains unavailable
because device open times out while reading the FPGA version.

Validation: production and test-injection libbladeRF builds; native
`libbladeRF_test_sync_epoch_traversal` in both builds; live-harness
`-Werror` syntax check; `git diff --check`. Tests cover timeout before sync
application copy, no first-host-data event at pre-copy admission, event commit
after the successful-copy boundary, and refusal to certify a late host packet.
Timeout remains a failure outcome and never makes IQ valid.
