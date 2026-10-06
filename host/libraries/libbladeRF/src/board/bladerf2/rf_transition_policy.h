/* Event requirement normalization for the bladeRF 2 RX transition API. */
#ifndef BLADERF2_RF_TRANSITION_POLICY_H_
#define BLADERF2_RF_TRANSITION_POLICY_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <libbladeRF.h>

static inline bool bladerf2_rf_transition_normalize_requirements(
    uint32_t requested, bool require_rx_data_valid, uint32_t *effective)
{
    const uint32_t supported = BLADERF_RF_REQUIRE_PLL_LOCKED |
                              BLADERF_RF_REQUIRE_ENSM_RX |
                              BLADERF_RF_REQUIRE_DATAPATH_ARMED |
                              BLADERF_RF_REQUIRE_EPOCH_VALID;

    if (effective == NULL || (requested & ~supported) != 0) {
        return false;
    }

    *effective = requested;
    if (require_rx_data_valid ||
        (requested & BLADERF_RF_REQUIRE_DATAPATH_ARMED) != 0) {
        *effective |= BLADERF_RF_REQUIRE_EPOCH_VALID;
    }

    if (*effective == 0) {
        return false;
    }

    if ((*effective & BLADERF_RF_REQUIRE_EPOCH_VALID) != 0) {
        *effective |= BLADERF_RF_REQUIRE_PLL_LOCKED |
                      BLADERF_RF_REQUIRE_ENSM_RX;
    }
    return true;
}

#endif
