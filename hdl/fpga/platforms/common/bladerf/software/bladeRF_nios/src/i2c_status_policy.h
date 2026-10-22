#ifndef BLADERF_NIOS_I2C_STATUS_POLICY_H_
#define BLADERF_NIOS_I2C_STATUS_POLICY_H_

#include <stdbool.h>
#include <stdint.h>

#include "nios_pkt_8x32.h"

/* OpenCores I2C status is a snapshot: TIP clears when the byte completes,
 * while RXACK retains that byte's ACK/NACK result until another command.
 * Never poll RXACK waiting for it to change after completion.
 *
 * Arbitration loss also clears TIP, but the controller abandoned the byte
 * (and any STOP) and RXACK still holds an older byte's result, so AL fails
 * the transfer whatever RXACK says.
 *
 * Returns 0 on success, otherwise the NIOS_I2C_FAIL_* reason. */
static inline uint8_t bladerf_i2c_status_failure(uint8_t status,
                                                 uint8_t tip_mask,
                                                 uint8_t rxack_mask,
                                                 uint8_t al_mask,
                                                 bool check_rxack)
{
    if ((status & tip_mask) != 0) {
        return NIOS_I2C_FAIL_TIP_CLEAR;
    }
    if ((status & al_mask) != 0) {
        return NIOS_I2C_FAIL_ARBITRATION;
    }
    if (check_rxack && (status & rxack_mask) != 0) {
        return NIOS_I2C_FAIL_NACK;
    }
    return 0;
}

#endif
