#include <stdint.h>
#include <unistd.h>

#include "io.h"
#include "system.h"
#include "altera_avalon_timer_regs.h"

#include "devices.h"
#include "platform.h"
#include "no_os_delay.h"

static bladerf_platform_service_fn service_hook;

void bladerf_platform_set_service_hook(bladerf_platform_service_fn hook)
{
    service_hook = hook;
}

void bladerf_platform_service(void)
{
    if (service_hook != NULL) {
        service_hook();
    }
}

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

#define BLADERF_PLATFORM_SERVICE_SLICE_US 1000U

static void bladerf_serviceable_delay_us(uint64_t usecs)
{
	/* A single ADI delay must not starve the NIOS command/status path for its
	 * full duration. Keep service opportunities bounded even for callers that
	 * request long waits (for example, multi-device synchronization). */
	if (usecs == 0) {
		bladerf_platform_service();
		return;
	}

	while (usecs != 0) {
		uint32_t slice = usecs > BLADERF_PLATFORM_SERVICE_SLICE_US
				 ? BLADERF_PLATFORM_SERVICE_SLICE_US
				 : (uint32_t)usecs;

		usleep(slice);
		bladerf_platform_service();
		usecs -= slice;
	}
}

void no_os_udelay(uint32_t usecs)
{
	bladerf_serviceable_delay_us(usecs);
}

void no_os_mdelay(uint32_t msecs)
{
	bladerf_serviceable_delay_us((uint64_t)msecs * 1000ULL);
}

/* Return elapsed time from a free-running 64-bit Avalon timer on sys_clock.
 * This clock is intentionally independent of usleep()/alt_busy_sleep(): the
 * latter is a calibrated CPU loop and cannot be used as an operation
 * deadline. The timer snapshot makes the four 16-bit reads atomic. */
struct no_os_time no_os_get_time(void)
{
    uint64_t ticks_down;
    uint64_t ticks;
    struct no_os_time now;

    IOWR_ALTERA_AVALON_TIMER_SNAP_0(RFIC_MONOTONIC_TIMER_BASE, 0);
    ticks_down = (uint64_t)IORD_ALTERA_AVALON_TIMER_SNAP_0(
                     RFIC_MONOTONIC_TIMER_BASE);
    ticks_down |= (uint64_t)IORD_ALTERA_AVALON_TIMER_SNAP_1(
                      RFIC_MONOTONIC_TIMER_BASE) << 16;
    ticks_down |= (uint64_t)IORD_ALTERA_AVALON_TIMER_SNAP_2(
                      RFIC_MONOTONIC_TIMER_BASE) << 32;
    ticks_down |= (uint64_t)IORD_ALTERA_AVALON_TIMER_SNAP_3(
                      RFIC_MONOTONIC_TIMER_BASE) << 48;

    ticks = UINT64_MAX - ticks_down;
    now.s = (unsigned int)(ticks / ALT_CPU_FREQ);
    now.us = (unsigned int)(((ticks % ALT_CPU_FREQ) * 1000000ULL) /
                            ALT_CPU_FREQ);
    return now;
}

/*******************************************************************************
 * @brief msleep_interruptible
 ******************************************************************************/

unsigned long msleep_interruptible(unsigned int msecs)
{
    usleep(msecs * 1000);
    return 0;
}
