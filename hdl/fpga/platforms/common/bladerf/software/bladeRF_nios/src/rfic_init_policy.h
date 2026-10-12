#ifndef BLADERF_NIOS_RFIC_INIT_POLICY_H_
#define BLADERF_NIOS_RFIC_INIT_POLICY_H_

#include <stdbool.h>
#include <stdint.h>

/* Standby preserves a live PHY and its saved TX mute state. Restore it only
 * when the transition really starts in standby with that PHY still present. */
static inline bool rfic_init_restore_standby_state(bool was_standby,
                                                   bool phy_present)
{
    return was_standby && phy_present;
}

/* Keep the original initialization stage when cleanup succeeds, but make a
 * failed cleanup visible through the RFIC STATUS stage field. */
static inline uint8_t rfic_init_cleanup_stage(int cleanup_status,
                                              uint8_t failure_stage,
                                              uint8_t cleanup_failed_stage)
{
    return cleanup_status < 0 ? cleanup_failed_stage : failure_stage;
}

#endif
