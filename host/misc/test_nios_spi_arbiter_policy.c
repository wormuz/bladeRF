#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "spi_arbiter_policy.h"

struct fake_arbiter {
    uint8_t grant_after;
    uint8_t release_after;
    uint8_t reads;
    uint8_t phase_reads;
    bool request_active;
    bool was_granted;
    bool release_stuck;
    unsigned int request_writes;
    unsigned int cancel_writes;
};

struct fake_deadline {
    unsigned int checks_before_expired;
    unsigned int checks;
};

static bool deadline_expired(void *context)
{
    struct fake_deadline *deadline = context;

    deadline->checks++;
    return deadline->checks >= deadline->checks_before_expired;
}

static void set_request(void *context, bool active)
{
    struct fake_arbiter *arbiter = context;

    arbiter->request_active = active;
    if (active) {
        arbiter->request_writes++;
        arbiter->phase_reads = 0;
    } else {
        arbiter->cancel_writes++;
        arbiter->phase_reads = 0;
    }
}

static uint8_t read_grant(void *context)
{
    struct fake_arbiter *arbiter = context;

    arbiter->reads++;
    arbiter->phase_reads++;
    if (arbiter->request_active) {
        if (arbiter->grant_after != 0 &&
            arbiter->phase_reads >= arbiter->grant_after) {
            arbiter->was_granted = true;
            return 1;
        }
        return 0;
    }
    if (arbiter->was_granted && arbiter->release_stuck) {
        return 1;
    }
    if (arbiter->was_granted && arbiter->release_after != 0 &&
        arbiter->phase_reads < arbiter->release_after) {
        return 1;
    }
    return 0;
}

int main(void)
{
    struct fake_arbiter delayed = { .grant_after = 3 };
    struct fake_arbiter absent = { 0 };
    struct fake_arbiter delayed_release = {
        .grant_after = 1,
        .release_after = 3,
    };
    struct fake_arbiter stuck_release = {
        .grant_after = 1,
        .release_stuck = true,
    };
    struct fake_arbiter timed_delayed = { .grant_after = 3 };
    struct fake_arbiter timed_absent = { 0 };
    struct fake_arbiter timed_stuck_release = {
        .grant_after = 1,
        .release_stuck = true,
    };
    struct fake_deadline not_expired = { .checks_before_expired = 3 };
    struct fake_deadline expires = { .checks_before_expired = 1 };
    struct fake_deadline release_expires = { .checks_before_expired = 1 };

    if (!bladerf_spi_arbiter_acquire(5, &delayed, set_request, read_grant) ||
        !delayed.request_active || delayed.cancel_writes != 0 ||
        delayed.request_writes != 1) {
        fputs("delayed SPI arbiter grant was mishandled\n", stderr);
        return 1;
    }

    if (!bladerf_spi_arbiter_acquire_until(
            1000, &timed_delayed, set_request, read_grant,
            deadline_expired, &not_expired) ||
        !timed_delayed.request_active || not_expired.checks != 2) {
        fputs("SPI arbiter grant before deadline was rejected\n", stderr);
        return 1;
    }

    if (bladerf_spi_arbiter_acquire(4, &absent, set_request, read_grant) ||
        absent.request_active || absent.request_writes != 1 ||
        absent.cancel_writes != 1 || absent.reads != 6) {
        fputs("timed-out SPI arbiter request was not cancelled\n", stderr);
        return 1;
    }

    if (bladerf_spi_arbiter_acquire_until(
            1000, &timed_absent, set_request, read_grant,
            deadline_expired, &expires) ||
        timed_absent.request_active || timed_absent.request_writes != 1 ||
        timed_absent.cancel_writes != 1 || expires.checks != 1) {
        fputs("SPI arbiter monotonic deadline did not cancel request\n",
              stderr);
        return 1;
    }

    if (!bladerf_spi_arbiter_acquire(5, &delayed_release,
                                     set_request, read_grant) ||
        !bladerf_spi_arbiter_release(5, &delayed_release,
                                     set_request, read_grant) ||
        delayed_release.request_active || delayed_release.cancel_writes != 1 ||
        delayed_release.phase_reads != 3) {
        fputs("delayed SPI arbiter release was not confirmed\n", stderr);
        return 1;
    }

    if (!bladerf_spi_arbiter_acquire(5, &stuck_release,
                                     set_request, read_grant) ||
        bladerf_spi_arbiter_release(3, &stuck_release,
                                    set_request, read_grant) ||
        bladerf_spi_arbiter_acquire(3, &stuck_release,
                                    set_request, read_grant) ||
        stuck_release.request_writes != 1) {
        fputs("stuck SPI grant was reused before release confirmation\n",
              stderr);
        return 1;
    }

    if (!bladerf_spi_arbiter_acquire(5, &timed_stuck_release,
                                     set_request, read_grant) ||
        bladerf_spi_arbiter_release_until(
            1000, &timed_stuck_release, set_request, read_grant,
            deadline_expired, &release_expires) ||
        timed_stuck_release.request_active ||
        timed_stuck_release.cancel_writes != 1 ||
        release_expires.checks != 1 ||
        bladerf_spi_arbiter_acquire(3, &timed_stuck_release,
                                    set_request, read_grant) ||
        timed_stuck_release.request_writes != 1) {
        fputs("expired SPI arbiter release was reused as a valid grant\n",
              stderr);
        return 1;
    }

    puts("NIOS SPI arbiter timeout cancellation policy: PASS");
    return 0;
}
