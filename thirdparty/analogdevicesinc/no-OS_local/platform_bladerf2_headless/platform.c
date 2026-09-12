#include <stdint.h>
#include <unistd.h>

#include "devices.h"
#include "platform.h"

/* SPI, GPIO and AXI IO now reach the driver through the no-OS platform ops
 * tables and no_os_axi_io, implemented in bladerf2_headless_spi.c,
 * bladerf2_headless_gpio.c and bladerf2_headless_axi_io.c respectively
 * (mirrors the host platform_bladerf2 split). adc_core.c/dac_core.c
 * (axiadc_read/write via the old struct axiadc_state API) are gone: the
 * current driver drives the AXI ADC/DAC cores itself through axi_adc_core.c/
 * axi_dac_core.c and axi_adc_read/write, which call into no_os_axi_io_read/
 * write above. */

/*******************************************************************************
 * @brief udelay
 ******************************************************************************/

void udelay(unsigned long usecs)
{
    usleep(usecs);
}

/*******************************************************************************
 * @brief mdelay
 ******************************************************************************/

void mdelay(unsigned long msecs)
{
    usleep(msecs * 1000);
}

/*******************************************************************************
 * @brief no_os_udelay, no_os_mdelay
 *
 * The current driver revision calls the delays through no_os_delay.h. Only
 * the names and the argument width changed, so these forward to the existing
 * implementations rather than duplicating them.
 ******************************************************************************/

void no_os_udelay(uint32_t usecs)
{
    udelay(usecs);
}

void no_os_mdelay(uint32_t msecs)
{
    mdelay(msecs);
}

/*******************************************************************************
 * @brief msleep_interruptible
 ******************************************************************************/

unsigned long msleep_interruptible(unsigned int msecs)
{
    usleep(msecs * 1000);
    return 0;
}
