/* Pure policy shared by the NIOS RFIC path and host-side regression source. */
#ifndef BLADERF_NIOS_RFIC_FREQUENCY_POLICY_H_
#define BLADERF_NIOS_RFIC_FREQUENCY_POLICY_H_

#include <stdbool.h>

static inline bool rfic_dependent_fdd_peer_frequency_unknown(
    bool dependent_fdd, bool tx_target, bool rx_frequency_known,
    bool tx_frequency_known)
{
    if (!dependent_fdd) {
        return false;
    }

    return tx_target ? !rx_frequency_known : !tx_frequency_known;
}

/* Only the two explicit LO writes during cold initialization may establish
 * a first peer frequency without a prior known peer; stale INIT state alone
 * never grants this exception to runtime commands. */
static inline bool rfic_frequency_change_blocked(bool init_sequence,
                                                 bool peer_unknown)
{
    return !init_sequence && peer_unknown;
}

/* In HOST tuning mode NIOS has no AD9361 PHY. A synchronous host-owned
 * fastlock recall may still use NIOS for the raw profile activation because
 * libbladeRF verifies the resulting LO through the active host PHY. Deferred
 * recalls have no such synchronous host readback and must retain NIOS PHY
 * ownership. */
static inline bool rfic_fastlock_host_owned_without_phy(bool immediate,
                                                         bool phy_present,
                                                         bool faulted)
{
    return immediate && !phy_present && !faulted;
}

#endif
