/*
 * This file is part of the bladeRF project:
 *   http://www.github.com/nuand/bladeRF
 *
 * Copyright (C) 2013 Nuand LLC
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "host_config.h"

#if BLADERF_OS_WINDOWS || BLADERF_OS_OSX
#include "clock_gettime.h"
#else
#include <time.h>
#endif

#include <stddef.h>
#include <libbladeRF.h>
#include "helpers/timeout.h"

int bladerf_monotonic_now_ns(uint64_t *now_ns)
{
    struct timespec now;

    if (now_ns == NULL || clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return BLADERF_ERR_UNEXPECTED;
    }

    *now_ns = (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
    return 0;
}

int bladerf_timeout_remaining_ms(uint64_t start_ns,
                                 unsigned int timeout_ms,
                                 unsigned int *remaining_ms)
{
    uint64_t now_ns, elapsed_ns, remaining_ns;
    int status;

    if (remaining_ms == NULL || timeout_ms == 0) {
        return BLADERF_ERR_INVAL;
    }

    status = bladerf_monotonic_now_ns(&now_ns);
    if (status != 0) {
        return status;
    }

    elapsed_ns = now_ns >= start_ns ? now_ns - start_ns : 0;
    if (elapsed_ns >= (uint64_t)timeout_ms * 1000000ULL) {
        return BLADERF_ERR_TIMEOUT;
    }

    remaining_ns = (uint64_t)timeout_ms * 1000000ULL - elapsed_ns;
    *remaining_ms = (unsigned int)((remaining_ns + 999999ULL) / 1000000ULL);
    return 0;
}

int populate_abs_timeout(struct timespec *t, unsigned int timeout_ms)
{
    static const int nsec_per_sec = 1000 * 1000 * 1000;
    const unsigned int timeout_sec = timeout_ms / 1000;
    int status;

    status = clock_gettime(CLOCK_REALTIME, t);
    if (status != 0) {
        return BLADERF_ERR_UNEXPECTED;
    } else {
        t->tv_sec += timeout_sec;
        t->tv_nsec += (timeout_ms % 1000) * 1000 * 1000;

        if (t->tv_nsec >= nsec_per_sec) {
            t->tv_sec += t->tv_nsec / nsec_per_sec;
            t->tv_nsec %= nsec_per_sec;
        }

        return 0;
    }
}
