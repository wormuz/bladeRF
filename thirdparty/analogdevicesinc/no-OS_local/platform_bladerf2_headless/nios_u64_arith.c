/*
 * Compact 64-bit division helpers for the NIOS II firmware.
 *
 * GCC's generic libgcc division routines use several kilobytes on this target.
 * AD9361 rate calculations need these operations only during control work, so
 * a small shift/subtract implementation trades some latency for firmware RAM.
 */

#include <stdint.h>

static uint64_t divmod_u64(uint64_t numerator, uint64_t denominator,
			   uint64_t *remainder)
{
	uint64_t quotient = 0;
	uint64_t bit = 1;

	if (denominator == 0) {
		if (remainder)
			*remainder = numerator;
		return UINT64_MAX;
	}
	if (denominator > numerator) {
		if (remainder)
			*remainder = numerator;
		return 0;
	}
	while (denominator <= (numerator >> 1)) {
		denominator <<= 1;
		bit <<= 1;
	}
	do {
		if (numerator >= denominator) {
			numerator -= denominator;
			quotient |= bit;
		}
		bit >>= 1;
		denominator >>= 1;
	} while (bit != 0);

	if (remainder)
		*remainder = numerator;
	return quotient;
}

uint64_t __udivdi3(uint64_t numerator, uint64_t denominator)
{
	return divmod_u64(numerator, denominator, NULL);
}

uint64_t __umoddi3(uint64_t numerator, uint64_t denominator)
{
	uint64_t remainder;

	(void)divmod_u64(numerator, denominator, &remainder);
	return remainder;
}

int64_t __divdi3(int64_t numerator, int64_t denominator)
{
	uint64_t unsigned_numerator = numerator < 0
		? 0 - (uint64_t)numerator : (uint64_t)numerator;
	uint64_t unsigned_denominator = denominator < 0
		? 0 - (uint64_t)denominator : (uint64_t)denominator;
	uint64_t quotient = divmod_u64(unsigned_numerator,
				       unsigned_denominator, NULL);

	if ((numerator < 0) != (denominator < 0))
		quotient = 0 - quotient;
	return (int64_t)quotient;
}

int64_t __moddi3(int64_t numerator, int64_t denominator)
{
	uint64_t unsigned_numerator = numerator < 0
		? 0 - (uint64_t)numerator : (uint64_t)numerator;
	uint64_t unsigned_denominator = denominator < 0
		? 0 - (uint64_t)denominator : (uint64_t)denominator;
	uint64_t remainder;

	(void)divmod_u64(unsigned_numerator, unsigned_denominator, &remainder);
	if (numerator < 0)
		remainder = 0 - remainder;
	return (int64_t)remainder;
}
