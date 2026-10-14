#ifndef BLADERF_NIOS_RFIC_INIT_POLICY_H_
#define BLADERF_NIOS_RFIC_INIT_POLICY_H_

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

/* Standby preserves a live PHY and its saved TX mute state. Restore it only
 * when the transition really starts in standby with that PHY still present. */
static inline bool rfic_init_restore_standby_state(bool was_standby,
                                                   bool phy_present)
{
    return was_standby && phy_present;
}

/* Resume after the retained ADI constructor only when it reached success and
 * published its candidate PHY. A terminal error must take the cleanup path. */
static inline bool rfic_init_adi_resume(bool job_started, bool job_pending,
                                        int32_t job_status, bool phy_present)
{
    return job_started && !job_pending && job_status == 0 && phy_present;
}

/* Keep the original initialization stage when cleanup succeeds, but make a
 * failed cleanup visible through the RFIC STATUS stage field. */
static inline uint8_t rfic_init_cleanup_stage(int cleanup_status,
                                              uint8_t failure_stage,
                                              uint8_t cleanup_failed_stage)
{
    return cleanup_status < 0 ? cleanup_failed_stage : failure_stage;
}

/* A boolean INIT failure must still have a concrete terminal cause. Preserve
 * negative driver errors; use EIO for invariant failures that lack errno. */
static inline int32_t rfic_init_failure_status(int32_t status)
{
    return status < 0 ? status : -EIO;
}

/* NIOS owns no AXI ADC capture handle. The BIST digital-interface tuner can
 * therefore not be used in this firmware; production must keep preset delays
 * enabled and gate tuner entry points before the RFIC is touched. */
static inline bool rfic_init_digital_tune_policy_valid(uint8_t skip_mode,
                                                       bool fir_disable)
{
    return skip_mode == 2 && fir_disable;
}

/* Disconnect verification must exclude RESETB: the RFFE helper clears RF
 * path controls while preserving the caller's asserted/deasserted reset
 * state, including during quarantine recovery. */
static inline uint32_t rfic_disconnect_verify_mask(uint32_t output_mask,
                                                   uint32_t reset_mask)
{
    return output_mask & ~reset_mask;
}

#endif
