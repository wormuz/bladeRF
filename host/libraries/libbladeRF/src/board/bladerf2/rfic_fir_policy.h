/*
 * This file is part of the bladeRF project.
 * Copyright (C) 2026 Nuand LLC
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#ifndef BLADERF2_RFIC_FIR_POLICY_H_
#define BLADERF2_RFIC_FIR_POLICY_H_

#include <stdbool.h>

#include <libbladeRF.h>

static inline int bladerf2_rfic_fir_cache_status(bool known)
{
    return known ? 0 : BLADERF_ERR_UNEXPECTED;
}

#endif
