/*
 * This file is part of the bladeRF project.
 * Copyright (C) 2026 Nuand LLC
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#ifndef BLADERF2_NIOS_RAM_POLICY_H_
#define BLADERF2_NIOS_RAM_POLICY_H_

#include <stdbool.h>
#include <stdint.h>

#include "nios_pkt_8x32.h"

/* v[] is indexed by NIOS_PKT_8x32_NIOS_RAM_*. The NIOS rescans its RAM for
 * every field, so a set read across a stack or heap change no longer adds
 * up to the fixed region. */
static inline bool bladerf2_nios_ram_usage_consistent(const uint32_t v[4])
{
    uint64_t sum = (uint64_t)v[NIOS_PKT_8x32_NIOS_RAM_UNTOUCHED] +
                   v[NIOS_PKT_8x32_NIOS_RAM_HEAP] +
                   v[NIOS_PKT_8x32_NIOS_RAM_STACK];

    return sum == v[NIOS_PKT_8x32_NIOS_RAM_REGION];
}

#endif
