#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "rfic_calibration_policy.h"

int main(void)
{
    const uint64_t calibrated_lo = 2400000000ULL;
    const uint64_t staging_lo = 70000000ULL;
    const uint64_t threshold = 100000000ULL;

    /* ad9361_init() completed RFDC at calibrated_lo. Cold-init's temporary
     * staging hop and return to that same LO must not repeat RFDC. */
    if (rfic_rfdc_calibration_required_for_tune(
            calibrated_lo, staging_lo, threshold, true, staging_lo) ||
        rfic_rfdc_calibration_required_for_tune(
            calibrated_lo, calibrated_lo, threshold, true, staging_lo)) {
        fputs("cold-init repeated RFDC after successful setup calibration\n",
              stderr);
        return 1;
    }

    /* The same large move during ordinary operation still requires RFDC;
     * an unknown calibration baseline must never suppress it either. */
    if (!rfic_rfdc_calibration_required_for_tune(
            calibrated_lo, staging_lo, threshold, false, staging_lo) ||
        !rfic_rfdc_calibration_required_for_tune(
            0, staging_lo, threshold, true, staging_lo)) {
        fputs("runtime or unknown-baseline RFDC requirement was suppressed\n",
              stderr);
        return 1;
    }

    return 0;
}
