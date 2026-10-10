#ifndef BLADERF_MISC_TIMESTAMP_ORDER_H_
#define BLADERF_MISC_TIMESTAMP_ORDER_H_

#include <stdbool.h>
#include <stdint.h>

/* Modulo-2^64 timestamp ordering. Comparisons are unambiguous only when the
 * compared values are less than 2^63 ticks apart. */
static inline bool timestamp_at_or_after(uint64_t timestamp, uint64_t boundary)
{
    return (timestamp - boundary) < (UINT64_C(1) << 63);
}

static inline bool timestamp_strictly_after(uint64_t timestamp,
                                            uint64_t boundary)
{
    const uint64_t delta = timestamp - boundary;
    return delta != 0 && delta < (UINT64_C(1) << 63);
}

#endif
