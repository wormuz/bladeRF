#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#include "i2c_status_policy.h"

#define TEST_I2C_TIP   (1u << 1)
#define TEST_I2C_RXACK (1u << 7)

int main(void)
{
    /* An ACK is success only after TIP clears. */
    assert(bladerf_i2c_status_success(0, TEST_I2C_TIP, TEST_I2C_RXACK,
                                      true));
    assert(!bladerf_i2c_status_success(TEST_I2C_TIP, TEST_I2C_TIP,
                                       TEST_I2C_RXACK, true));

    /* RXACK is the completed byte's latched result: NACK fails immediately. */
    assert(!bladerf_i2c_status_success(TEST_I2C_RXACK, TEST_I2C_TIP,
                                       TEST_I2C_RXACK, true));

    /* The final read byte is intentionally NACKed by the I2C master. */
    assert(bladerf_i2c_status_success(TEST_I2C_RXACK, TEST_I2C_TIP,
                                      TEST_I2C_RXACK, false));
    return 0;
}
