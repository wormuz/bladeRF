# NIOS RFIC INIT stage is now retained by the host status path

Date: 2026-10-09

## Change

The NIOS RFIC status word already carries `_rfic_initialize()`'s last entered stage in bits 7:2. `rfic_fpga.c` previously decoded initialized, last-write-success, and queue length but discarded this field. The host now retains `init_stage` and includes its numeric value and symbolic name when a drained RFIC command reports failure.

This improves diagnosis when NIOS remains responsive long enough to return status after a failed INIT. It cannot reveal a stage after NIOS has stopped servicing USB/control requests; the currently preserved no-response incident therefore remains unresolved and untouched.

## Verification

- Full configured host CMake build completed successfully.
- `host/build/output/libbladeRF_test_sync_epoch_traversal` exited 0. Its expected fault-path cases print timeout diagnostics during the test.
- `ctest -R ...` found no registered tests in this build tree; the executable was run directly.
- No board access, FPGA reload, USB reset, or power cycle was performed.
