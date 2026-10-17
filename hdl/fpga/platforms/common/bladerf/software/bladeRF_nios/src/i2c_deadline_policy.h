#ifndef BLADERF_NIOS_I2C_DEADLINE_POLICY_H_
#define BLADERF_NIOS_I2C_DEADLINE_POLICY_H_

#include <stdbool.h>
#include <stdint.h>

/* Unsigned subtraction keeps elapsed-tick comparisons valid across wrap. */
static inline bool bladerf_i2c_deadline_expired_at(uint64_t now_ticks,
                                                   uint64_t start_ticks,
                                                   uint64_t timeout_ticks)
{
    return now_ticks - start_ticks >= timeout_ticks;
}

#endif
