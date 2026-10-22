#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#include "i2c_deadline_policy.h"
#include "i2c_status_policy.h"

#define TEST_I2C_TIP   (1u << 1)
#define TEST_I2C_AL    (1u << 5)
#define TEST_I2C_RXACK (1u << 7)

static uint8_t failure(uint8_t status, bool check_rxack)
{
    return bladerf_i2c_status_failure(status, TEST_I2C_TIP, TEST_I2C_RXACK,
                                      TEST_I2C_AL, check_rxack);
}

int main(void)
{
    /* An ACK is success only after TIP clears. */
    assert(failure(0, true) == 0);
    assert(failure(TEST_I2C_TIP, true) == NIOS_I2C_FAIL_TIP_CLEAR);

    /* RXACK is the completed byte's latched result: NACK fails immediately. */
    assert(failure(TEST_I2C_RXACK, true) == NIOS_I2C_FAIL_NACK);

    /* The final read byte is intentionally NACKed by the I2C master. */
    assert(failure(TEST_I2C_RXACK, false) == 0);

    /* Arbitration loss clears TIP and leaves an older RXACK; it fails even
     * when that stale RXACK reads as ACK, and on the NACKed final read. */
    assert(failure(TEST_I2C_AL, true) == NIOS_I2C_FAIL_ARBITRATION);
    assert(failure(TEST_I2C_AL | TEST_I2C_RXACK, false) ==
           NIOS_I2C_FAIL_ARBITRATION);

    /* The raw-tick deadline comparison is exact at the boundary and wraps. */
    assert(!bladerf_i2c_deadline_expired_at(100, 100, 10));
    assert(!bladerf_i2c_deadline_expired_at(109, 100, 10));
    assert(bladerf_i2c_deadline_expired_at(110, 100, 10));
    assert(!bladerf_i2c_deadline_expired_at(UINT64_MAX - 4,
                                            UINT64_MAX - 9, 10));
    assert(bladerf_i2c_deadline_expired_at(3, UINT64_MAX - 4, 8));
    return 0;
}
