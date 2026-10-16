#include <assert.h>

#include "rfic_frequency_policy.h"

int main(void)
{
    /* HOST mode may use NIOS for raw profile activation. Host LO readback
     * and TXQ calibration keep the transition invalid until verified. */
    assert(rfic_fastlock_host_owned_without_phy(false, false));

    /* An initialized NIOS PHY keeps the existing NIOS-owned path. */
    assert(!rfic_fastlock_host_owned_without_phy(true, false));
    /* A latched fault is not equivalent to an absent HOST-mode PHY. */
    assert(!rfic_fastlock_host_owned_without_phy(true, true));
    assert(!rfic_fastlock_host_owned_without_phy(false, true));
    return 0;
}
