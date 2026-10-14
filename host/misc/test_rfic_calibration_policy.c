#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "rfic_calibration_policy.h"

int main(void)
{
    const uint64_t calibrated_lo = 2400000000ULL;
    const uint64_t distant_lo = 70000000ULL;
    const uint64_t threshold = 100000000ULL;

    /* Ordinary large moves and an unknown baseline still require RFDC. */
    if (!rfic_rfdc_calibration_required(
            calibrated_lo, distant_lo, threshold) ||
        !rfic_rfdc_calibration_required(0, distant_lo, threshold)) {
        fputs("runtime or unknown-baseline RFDC requirement was suppressed\n",
              stderr);
        return 1;
    }

    return 0;
}
