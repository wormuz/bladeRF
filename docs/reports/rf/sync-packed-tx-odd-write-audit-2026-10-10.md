# Synchronous packed RX/TX odd-read/write audit — 2026-10-10

## Finding

The SC16_Q11_PACKED synchronous TX loop packed two complex samples per
iteration but accepted arbitrary `num_samples` per call. An odd final sample
made it read and write one sample past the caller and destination buffers. The
RX unpacker had the same two-sample assumption and could overrun both sides on
an odd read. It also used unaligned `uint16_t` loads when starting at an odd
packed sample offset. Both paths used signed shifts for sign extension or
packing, which depended on implementation behavior.

## Change

Packed TX conversion now emits each complex sample as three explicit
little-endian bytes. Packed RX conversion reads individual bytes and explicitly
sign-extends each 12-bit I/Q component. Both handle odd-sized calls and
unaligned packed offsets. The public sync calls also reject sample counts that
cannot be represented as host SC16 buffer byte offsets on narrow hosts. A
regression source checks exact bytes and decoded values for odd-sized input,
including negative I/Q values, with canaries around source/destination and a
decode beginning at an odd packed sample offset.

## Verification status

`git diff --check` passes. The regression has not been built or run because
the complete RX+TX source sweep still gates all builds and tests.
