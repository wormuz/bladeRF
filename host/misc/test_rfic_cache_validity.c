#include <stdbool.h>
#include <stdio.h>

#include "rfic_cache_validity.h"

int main(void)
{
    bool sample_rate_invalid[2] = { false, false };
    bool bandwidth_invalid[2] = { false, false };

    rfic_cache_invalidate_all(sample_rate_invalid, 2);
    rfic_cache_invalidate_all(bandwidth_invalid, 2);
    if (rfic_cache_read_is_valid(sample_rate_invalid, 2, 0) ||
        rfic_cache_read_is_valid(sample_rate_invalid, 2, 1) ||
        rfic_cache_read_is_valid(bandwidth_invalid, 2, 0) ||
        rfic_cache_read_is_valid(bandwidth_invalid, 2, 1) ||
        rfic_cache_read_is_valid(sample_rate_invalid, 2, 2)) {
        fputs("shared RF configuration cache remained readable\n", stderr);
        return 1;
    }

    rfic_cache_validate_all(sample_rate_invalid, 2);
    rfic_cache_validate_all(bandwidth_invalid, 2);
    if (!rfic_cache_read_is_valid(sample_rate_invalid, 2, 0) ||
        !rfic_cache_read_is_valid(sample_rate_invalid, 2, 1) ||
        !rfic_cache_read_is_valid(bandwidth_invalid, 2, 0) ||
        !rfic_cache_read_is_valid(bandwidth_invalid, 2, 1)) {
        fputs("successful shared RF update did not restore readback\n", stderr);
        return 1;
    }

    return 0;
}
