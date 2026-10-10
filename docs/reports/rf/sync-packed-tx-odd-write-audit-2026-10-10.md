# Synchronous packed TX odd-write audit — 2026-10-10

## Finding

The SC16_Q11_PACKED synchronous TX loop packed two complex samples per
iteration but accepted arbitrary `num_samples` per call. An odd final sample
made it read and write one sample past the caller and destination buffers. The
implementation also used potentially unaligned `uint16_t` stores and left
signed shifts of negative IQ values in the encoding path.

## Change

Packed conversion now emits each complex sample as three explicit
little-endian bytes. This handles odd-sized writes, avoids unaligned stores,
and masks signed input to its 12-bit representation. A regression source
checks the exact bytes for three samples, including negative I/Q values, with
canaries around the destination.

## Verification status

`git diff --check` passes. The regression has not been built or run because
the complete RX+TX source sweep still gates all builds and tests.
