/* Public channel validation shared by the generic API and source policy tests. */
#ifndef BLADERF_CHANNEL_POLICY_H_
#define BLADERF_CHANNEL_POLICY_H_

#include <stdbool.h>

#include <libbladeRF.h>

static inline bool bladerf_public_channel_valid(bladerf_channel channel)
{
    return channel == BLADERF_CHANNEL_RX(0) ||
           channel == BLADERF_CHANNEL_RX(1) ||
           channel == BLADERF_CHANNEL_TX(0) ||
           channel == BLADERF_CHANNEL_TX(1);
}

#endif
