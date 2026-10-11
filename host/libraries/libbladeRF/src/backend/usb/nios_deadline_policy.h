/* Fail-closed deadline arithmetic for NIOS control transactions. */
#ifndef BLADERF_NIOS_DEADLINE_POLICY_H_
#define BLADERF_NIOS_DEADLINE_POLICY_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static inline bool nios_deadline_start(int clock_status, uint64_t now_ns,
                                       uint64_t interval_ns,
                                       uint64_t *deadline_ns)
{
    if (clock_status != 0 || deadline_ns == NULL ||
        interval_ns > UINT64_MAX - now_ns) {
        return false;
    }

    *deadline_ns = now_ns + interval_ns;
    return true;
}

static inline bool nios_deadline_check(int clock_status, uint64_t now_ns,
                                       uint64_t deadline_ns, bool *expired)
{
    if (clock_status != 0 || expired == NULL) {
        return false;
    }

    *expired = now_ns >= deadline_ns;
    return true;
}

#endif
