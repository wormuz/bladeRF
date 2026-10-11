#ifndef BLADERF_NIOS_RFIC_CACHE_VALIDITY_H_
#define BLADERF_NIOS_RFIC_CACHE_VALIDITY_H_

#include <stdbool.h>
#include <stddef.h>

static inline void rfic_cache_invalidate_all(bool *invalid, size_t count)
{
    size_t i;

    for (i = 0; i < count; ++i) {
        invalid[i] = true;
    }
}

static inline void rfic_cache_validate_all(bool *invalid, size_t count)
{
    size_t i;

    for (i = 0; i < count; ++i) {
        invalid[i] = false;
    }
}

static inline bool rfic_cache_read_is_valid(const bool *invalid,
                                             size_t count,
                                             size_t direction)
{
    return direction < count && !invalid[direction];
}

#endif
