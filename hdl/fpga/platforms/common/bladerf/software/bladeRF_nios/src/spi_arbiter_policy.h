/* Copyright (c) 2026 Nuand LLC */

#ifndef BLADERF_SPI_ARBITER_POLICY_H_
#define BLADERF_SPI_ARBITER_POLICY_H_

#include <stdbool.h>
#include <stdint.h>

typedef void (*bladerf_spi_arbiter_request_fn)(void *context, bool active);
typedef uint8_t (*bladerf_spi_arbiter_grant_fn)(void *context);

/* The request callback must clear the request and acknowledge release when
 * called with active=false. This prevents a timed-out waiter from acquiring
 * the bus later as an orphaned requester. */
static inline bool bladerf_spi_arbiter_acquire(
    uint32_t tries, void *context,
    bladerf_spi_arbiter_request_fn set_request,
    bladerf_spi_arbiter_grant_fn read_grant)
{
    uint32_t count;

    if (set_request == 0 || read_grant == 0) {
        return false;
    }

    set_request(context, true);
    for (count = 0; count < tries; ++count) {
        if ((read_grant(context) & 1u) != 0) {
            return true;
        }
    }

    set_request(context, false);
    return false;
}

#endif
