/* Copyright (C) 2026 Nuand LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef BLADERF_NIOS_RFIC_CHANNEL_VALIDATION_H_
#define BLADERF_NIOS_RFIC_CHANNEL_VALIDATION_H_

#include <stdbool.h>
#include <stdint.h>

#include "libbladeRF.h"

#define RFIC_SYSTEM_CHANNEL 0xF

/* RFIC command addresses carry the command in bits 3:0 and the channel in
 * bits 11:8. Reject reserved bits instead of silently masking them into a
 * different command or channel. */
static inline bool _rfic_address_fields_valid(uint16_t address)
{
    return (address & UINT16_C(0xF0F0)) == 0;
}

/* A 32-bit command travels in the same 64-bit packet value as frequency and
 * status commands. Reject high bits before dispatch instead of silently
 * applying a different value after narrowing. */
static inline bool _rfic_write32_payload_valid(uint64_t value)
{
    return (value >> 32) == 0;
}

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
