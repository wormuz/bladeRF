#include <assert.h>

#include "rfic_frequency_policy.h"

int main(void)
{
    /* Host mode can use NIOS only for an immediate profile recall; the host
     * verifies the resulting LO before the transition becomes valid. */
    assert(rfic_fastlock_host_owned_without_phy(true, false));

    /* Deferred recalls have no synchronous host-side readback and therefore
     * require an initialized NIOS PHY. */
    assert(!rfic_fastlock_host_owned_without_phy(false, false));

    /* An initialized NIOS PHY keeps the existing NIOS-owned path. */
    assert(!rfic_fastlock_host_owned_without_phy(true, true));
    assert(!rfic_fastlock_host_owned_without_phy(false, true));
    return 0;
}
