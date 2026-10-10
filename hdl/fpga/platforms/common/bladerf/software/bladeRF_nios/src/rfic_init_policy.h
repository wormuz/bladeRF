#ifndef BLADERF_NIOS_RFIC_INIT_POLICY_H_
#define BLADERF_NIOS_RFIC_INIT_POLICY_H_

#include <stdbool.h>

/* Standby preserves a live PHY and its saved TX mute state. Restore it only
 * when the transition really starts in standby with that PHY still present. */
static inline bool rfic_init_restore_standby_state(bool was_standby,
                                                   bool phy_present)
{
    return was_standby && phy_present;
}

#endif
