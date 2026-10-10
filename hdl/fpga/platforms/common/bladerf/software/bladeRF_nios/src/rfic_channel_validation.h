/* Copyright (C) 2026 Nuand LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef BLADERF_NIOS_RFIC_CHANNEL_VALIDATION_H_
#define BLADERF_NIOS_RFIC_CHANNEL_VALIDATION_H_

#include <stdbool.h>

#include "libbladeRF.h"

#define RFIC_SYSTEM_CHANNEL 0xF

static inline bool _rfic_chan_is_system(bladerf_channel ch)
{
    return RFIC_SYSTEM_CHANNEL == ch;
}

static inline bool _rfic_chan_is_valid(bladerf_channel ch)
{
    return ch == BLADERF_CHANNEL_RX(0) ||
           ch == BLADERF_CHANNEL_TX(0) ||
           ch == BLADERF_CHANNEL_RX(1) ||
           ch == BLADERF_CHANNEL_TX(1) ||
           _rfic_chan_is_system(ch);
}

#endif
