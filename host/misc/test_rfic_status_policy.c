#include <stdbool.h>
#include <stdio.h>

#include "board/bladerf2/rfic_status_policy.h"

int main(void)
{
    /* Queue reset's 0xFF sentinel has a set low success bit, but is not a
     * successful command completion until the explicit valid bit is set. */
    if (bladerf2_rfic_status_result_accepted(true, false, true)) {
        fputs("queue-reset sentinel was accepted as a successful RFIC write\n",
              stderr);
        return 1;
    }
    if (!bladerf2_rfic_status_result_accepted(true, true, true)) {
        fputs("retired successful RFIC write was rejected\n", stderr);
        return 1;
    }
    if (bladerf2_rfic_status_result_accepted(true, true, false)) {
        fputs("retired failed RFIC write was accepted\n", stderr);
        return 1;
    }

    /* Pre-0.17 FPGA images do not implement the validity bit. Preserve the
     * prior success-result contract for those images. */
    if (!bladerf2_rfic_status_result_accepted(false, false, true) ||
        bladerf2_rfic_status_result_accepted(false, false, false)) {
        fputs("legacy FPGA RFIC result compatibility regressed\n", stderr);
        return 1;
    }

    return 0;
}
