#include <stdbool.h>
#include <stdio.h>

#include "board/bladerf2/rfic_status_policy.h"

int main(void)
{
    if (bladerf2_rfic_status_matches_retirement(0, true, 1, 0) ||
        bladerf2_rfic_status_matches_retirement(1, true, 1, 1) ||
        !bladerf2_rfic_status_matches_retirement(0, true, 1, 1) ||
        !bladerf2_rfic_status_matches_retirement(0, false, 1, 0)) {
        fputs("RFIC retirement wait accepted a stale or pending status\n",
              stderr);
        return 1;
    }

    /* Queue reset's 0xFF sentinel has a set low success bit, but is not a
     * successful command completion until the explicit valid bit is set. */
    if (bladerf2_rfic_status_result_accepted(true, false, true,
                                              true, 1, 0)) {
        fputs("queue-reset sentinel was accepted as a successful RFIC write\n",
              stderr);
        return 1;
    }
    if (!bladerf2_rfic_status_result_accepted(true, true, true,
                                               true, 1, 1)) {
        fputs("retired successful RFIC write was rejected\n", stderr);
        return 1;
    }
    if (bladerf2_rfic_status_result_accepted(true, true, false,
                                              true, 1, 1)) {
        fputs("retired failed RFIC write was accepted\n", stderr);
        return 1;
    }

    /* Pre-0.17 FPGA images do not implement the validity bit. Preserve the
     * prior success-result contract for those images. */
    if (bladerf2_rfic_status_result_accepted(true, true, true,
                                              true, 2, 1)) {
        fputs("stale successful RFIC command certified a later request\n",
              stderr);
        return 1;
    }
    if (!bladerf2_rfic_status_result_accepted(true, true, true,
                                               true, 0, 0)) {
        fputs("RFIC command sequence wraparound was rejected\n", stderr);
        return 1;
    }

    if (!bladerf2_rfic_status_result_accepted(false, false, true,
                                               false, 0, 0) ||
        bladerf2_rfic_status_result_accepted(false, false, false,
                                              false, 0, 0)) {
        fputs("legacy FPGA RFIC result compatibility regressed\n", stderr);
        return 1;
    }

    return 0;
}
