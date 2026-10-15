#include <stdint.h>
#include <sys/time.h>

#include "board/board.h"

#include "platform.h"
#include "no_os_delay.h"


/***************************************************************************//**
 * @brief udelay
*******************************************************************************/

void udelay(unsigned long usecs)
{
    usleep(usecs);
}

/***************************************************************************//**
 * @brief mdelay
*******************************************************************************/

void mdelay(unsigned long msecs)
{
    usleep(msecs * 1000);
}

/***************************************************************************//**
 * @brief no_os_udelay, no_os_mdelay
 *
 * The current driver revision calls the delays through no_os_delay.h. Only the
 * names and the argument width changed, so these forward to the existing
 * implementations rather than duplicating them.
*******************************************************************************/

void no_os_udelay(uint32_t usecs)
{
    udelay(usecs);
}

void no_os_mdelay(uint32_t msecs)
{
    mdelay(msecs);
}

/***************************************************************************//**
 * @brief msleep_interruptible
*******************************************************************************/

unsigned long msleep_interruptible(unsigned int msecs)
{
    usleep(msecs * 1000);
    return 0;
}

/* ADI's logging helpers reference this hook even in host builds. The NIOS
 * platform supplies a monotonic Avalon timer; host diagnostics only need a
 * timestamp, so use the platform wall clock here. */
struct no_os_time no_os_get_time(void)
{
    struct timeval now;
    struct no_os_time result = {0, 0};

    if (gettimeofday(&now, NULL) == 0) {
        result.s = (unsigned int)now.tv_sec;
        result.us = (unsigned int)now.tv_usec;
    }
    return result;
}

/*
 * The axiadc_* accessors that used to live here are gone: they served the AXI
 * ADC/DAC cores, which this board does not have. config.h now defines
 * AXI_ADC_NOT_PRESENT, so the driver never calls into that path, and
 * adc_core.c / dac_core.c are out of the build for the same reason.
 */
