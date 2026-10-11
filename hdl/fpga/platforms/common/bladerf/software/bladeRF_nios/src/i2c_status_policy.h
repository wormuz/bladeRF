#ifndef BLADERF_NIOS_I2C_STATUS_POLICY_H_
#define BLADERF_NIOS_I2C_STATUS_POLICY_H_

#include <stdbool.h>
#include <stdint.h>

/* OpenCores I2C status is a snapshot: TIP clears when the byte completes,
 * while RXACK retains that byte's ACK/NACK result until another command.
 * Never poll RXACK waiting for it to change after completion. */
static inline bool bladerf_i2c_status_success(uint8_t status,
                                               uint8_t tip_mask,
                                               uint8_t rxack_mask,
                                               bool check_rxack)
{
    return (status & tip_mask) == 0 &&
           (!check_rxack || (status & rxack_mask) == 0);
}

#endif
