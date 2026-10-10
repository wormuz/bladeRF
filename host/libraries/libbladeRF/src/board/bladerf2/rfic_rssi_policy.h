/*
 * This file is part of the bladeRF project.
 * Copyright (C) 2026 Nuand LLC
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#ifndef BLADERF2_RFIC_RSSI_POLICY_H_
#define BLADERF2_RFIC_RSSI_POLICY_H_

#include <stdbool.h>
#include <stdint.h>

#include <libbladeRF.h>

static inline bool bladerf2_rssi_channel_known(bladerf_channel ch)
{
    return ch == BLADERF_CHANNEL_RX(0) || ch == BLADERF_CHANNEL_RX(1) ||
           ch == BLADERF_CHANNEL_TX(0) || ch == BLADERF_CHANNEL_TX(1);
}

static inline bool bladerf2_rssi_channel_supported(bladerf_channel ch)
{
    /* bladeRF 2.0 micro has no TX_MON coupler: both inputs are grounded. */
    return ch == BLADERF_CHANNEL_RX(0) || ch == BLADERF_CHANNEL_RX(1);
}

/* TX RSSI reads enable the TX monitor path, which can also be selected as
 * the AD9361 RX input. Treat this diagnostic read as an RX RF-port change. */
static inline bool bladerf2_rssi_may_change_rx_path(bladerf_channel ch)
{
    return BLADERF_CHANNEL_IS_TX(ch);
}

static inline bool bladerf2_rssi_multiplier_valid(int16_t multiplier)
{
    return multiplier > 0;
}

#endif
