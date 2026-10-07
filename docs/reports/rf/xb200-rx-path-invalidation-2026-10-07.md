# XB-200 RF path setters now revoke the active RX epoch

The public XB-200 filterbank and path APIs changed the physical RX route
without passing through the common RX invalidation path. A certified META
stream could therefore remain marked valid across an external filter/path
change.

`bladerf_xb200_set_filterbank()` and `bladerf_xb200_set_path()` now revoke
the selected RX epoch before the hardware operation and release the setter
reservation through the usual completion callback, including when the
underlying expansion operation fails. TX-only XB-200 changes do not invalidate
RX. The existing `RF_PORT` reason identifies these external RF path/filter
changes; no new event bits were allocated, preserving the event flag ABI.

The mock-backend regression verifies RX filterbank and RX bypass/mixer path
invalidation, that a failed path fence prevents LMS/GPIO writes, and that
TX-only filter changes leave the RX epoch untouched. The audit of generic
libbladeRF RF setters found no other missing invalidation hooks; direct
expansion RF-path control was the gap. Python/Cython already maps the
`RF_PORT` reason. This repository does not
expose XB-200 mutators through its Python API, so the change is in the public C
API and native event path.

Validation: production `libbladerf_shared` build, native
`libbladeRF_test_rx_shared_clock_invalidation`, native sync epoch traversal,
Cython extension rebuild against this fork, 15 Python RF-event tests, and
`git diff --check`. Hardware qualification was not run because device open
currently times out while reading the FPGA version. No timeout or discard
establishes IQ validity.
