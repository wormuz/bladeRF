#ifndef BLADERF_RFIC_TX_OUTPUT_POLICY_H_
#define BLADERF_RFIC_TX_OUTPUT_POLICY_H_

#include <stdint.h>

/* Build a read-modify-write value that disconnects only TX at the RFIC/RFFE
 * boundary while preserving RX and unrelated board controls. */
static inline uint32_t rfic_tx_disconnect_value(uint32_t current,
                                                 uint32_t tx_control_mask,
                                                 uint32_t tx_lane_mask,
                                                 uint32_t tx_switch_mask)
{
    return current & ~(tx_control_mask | tx_lane_mask | tx_switch_mask);
}

#endif
