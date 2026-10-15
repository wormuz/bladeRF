/* Copyright (c) 2026 Nuand LLC */

#ifndef BLADERF_SPI_ARBITER_POLICY_H_
#define BLADERF_SPI_ARBITER_POLICY_H_

#include <stdbool.h>
#include <stdint.h>

typedef void (*bladerf_spi_arbiter_request_fn)(void *context, bool active);
typedef uint8_t (*bladerf_spi_arbiter_grant_fn)(void *context);
typedef bool (*bladerf_spi_arbiter_expired_fn)(void *context);

static inline bool bladerf_spi_arbiter_release_until(
    uint32_t tries, void *context,
    bladerf_spi_arbiter_request_fn set_request,
    bladerf_spi_arbiter_grant_fn read_grant,
    bladerf_spi_arbiter_expired_fn expired, void *deadline_context)
{
    uint32_t count;

    if (set_request == 0 || read_grant == 0) {
        return false;
    }

    set_request(context, false);
    for (count = 0; count < tries; ++count) {
        if ((read_grant(context) & 1u) == 0) {
            return expired == 0 || !expired(deadline_context);
        }
        if (expired != 0 && (count & 0x3fu) == 0 &&
            expired(deadline_context)) {
            return false;
        }
    }

    return false;
}

static inline bool bladerf_spi_arbiter_release(
    uint32_t tries, void *context,
    bladerf_spi_arbiter_request_fn set_request,
    bladerf_spi_arbiter_grant_fn read_grant)
{
    return bladerf_spi_arbiter_release_until(
        tries, context, set_request, read_grant, 0, 0);
}

/* The request callback must clear the request and acknowledge release when
 * called with active=false. This prevents a timed-out waiter from acquiring
 * the bus later as an orphaned requester. */
static inline bool bladerf_spi_arbiter_acquire_until(
    uint32_t tries, void *context,
    bladerf_spi_arbiter_request_fn set_request,
    bladerf_spi_arbiter_grant_fn read_grant,
    bladerf_spi_arbiter_expired_fn expired, void *deadline_context)
{
    uint32_t count;

    if (set_request == 0 || read_grant == 0) {
        return false;
    }

    /* A previous release may have been delayed by the arbiter clock. Do not
     * reassert request until its old grant is observably retired. */
    if ((read_grant(context) & 1u) != 0 &&
        !bladerf_spi_arbiter_release_until(tries, context, set_request,
                                           read_grant, expired,
                                           deadline_context)) {
        return false;
    }

    set_request(context, true);
    for (count = 0; count < tries; ++count) {
        if ((read_grant(context) & 1u) != 0) {
            if (expired != 0 && expired(deadline_context)) {
                /* Do not report a grant observed after the deadline as a
                 * successful acquisition. Drop the request and make one
                 * bounded release observation; a later lock must still
                 * refuse the bus if that release is not visible yet. */
                (void)bladerf_spi_arbiter_release_until(
                    1, context, set_request, read_grant, 0, 0);
                return false;
            }
            return true;
        }
        if (expired != 0 && (count & 0x3fu) == 0 &&
            expired(deadline_context)) {
            (void)bladerf_spi_arbiter_release_until(
                1, context, set_request, read_grant, 0, 0);
            return false;
        }
    }

    (void)bladerf_spi_arbiter_release_until(tries, context, set_request,
                                           read_grant, expired,
                                           deadline_context);
    return false;
}

static inline bool bladerf_spi_arbiter_acquire(
    uint32_t tries, void *context,
    bladerf_spi_arbiter_request_fn set_request,
    bladerf_spi_arbiter_grant_fn read_grant)
{
    return bladerf_spi_arbiter_acquire_until(
        tries, context, set_request, read_grant, 0, 0);
}

#endif
