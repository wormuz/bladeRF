#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "spi_arbiter_policy.h"

struct fake_arbiter {
    uint8_t grant_after;
    uint8_t reads;
    bool request_active;
    unsigned int request_writes;
    unsigned int cancel_writes;
};

static void set_request(void *context, bool active)
{
    struct fake_arbiter *arbiter = context;

    arbiter->request_active = active;
    if (active) {
        arbiter->request_writes++;
    } else {
        arbiter->cancel_writes++;
    }
}

static uint8_t read_grant(void *context)
{
    struct fake_arbiter *arbiter = context;

    arbiter->reads++;
    if (arbiter->grant_after != 0 &&
        arbiter->reads >= arbiter->grant_after) {
        return 1;
    }
    return 0;
}

int main(void)
{
    struct fake_arbiter delayed = { .grant_after = 3 };
    struct fake_arbiter absent = { 0 };

    if (!bladerf_spi_arbiter_acquire(5, &delayed, set_request, read_grant) ||
        !delayed.request_active || delayed.cancel_writes != 0 ||
        delayed.request_writes != 1) {
        fputs("delayed SPI arbiter grant was mishandled\n", stderr);
        return 1;
    }

    if (bladerf_spi_arbiter_acquire(4, &absent, set_request, read_grant) ||
        absent.request_active || absent.request_writes != 1 ||
        absent.cancel_writes != 1 || absent.reads != 4) {
        fputs("timed-out SPI arbiter request was not cancelled\n", stderr);
        return 1;
    }

    puts("NIOS SPI arbiter timeout cancellation policy: PASS");
    return 0;
}
