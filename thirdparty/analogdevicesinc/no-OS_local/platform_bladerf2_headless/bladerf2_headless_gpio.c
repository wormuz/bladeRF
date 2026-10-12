/*
 * GPIO back end for the AD9361 driver, as a no-OS platform ops table, for the
 * Nios softcore build.
 *
 * Mirrors bladerf2_gpio.c (host variant). The AD9361 control lines the driver
 * drives (resetb, sync) are bits of the FPGA's RFFE control register, read
 * via rffe_csr_read()/rffe_csr_write() (devices.c, common bladeRF_nios app
 * code) instead of the host's USB backend rffe_control_read/write. The bit
 * number travels in desc->number, same as the host variant; there is no
 * device handle to carry in desc->extra on this platform.
 */
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>

#include "devices.h"
#include "devices_inline.h"

#include "no_os_gpio.h"

static int32_t bladerf2_headless_gpio_get(struct no_os_gpio_desc **desc,
                                          const struct no_os_gpio_init_param *param)
{
    struct no_os_gpio_desc *d;

    if (NULL == desc || NULL == param) {
        return -EINVAL;
    }
    if (param->number < 0 || param->number >= 32) {
        return -EINVAL;
    }

    d = calloc(1, sizeof(*d));
    if (NULL == d) {
        return -ENOMEM;
    }

    d->port = param->port;
    d->number = param->number;
    d->pull = param->pull;
    d->platform_ops = param->platform_ops;
    d->extra = param->extra;

    *desc = d;

    return 0;
}

static int32_t bladerf2_headless_gpio_get_optional(struct no_os_gpio_desc **desc,
                                                    const struct no_os_gpio_init_param *param)
{
    /* An absent line is not an error: the driver treats a NULL descriptor as
     * "this board does not wire that signal". */
    if (NULL == desc) {
        return -EINVAL;
    }
    if (NULL == param || param->number < 0) {
        *desc = NULL;
        return 0;
    }

    return bladerf2_headless_gpio_get(desc, param);
}

static int32_t bladerf2_headless_gpio_remove(struct no_os_gpio_desc *desc)
{
    free(desc);

    return 0;
}

static int32_t bladerf2_headless_gpio_direction_output(struct no_os_gpio_desc *desc,
                                                        uint8_t value)
{
    if (NULL == desc) {
        return -EINVAL;
    }

    return no_os_gpio_set_value(desc, value);
}

static int32_t bladerf2_headless_gpio_direction_input(struct no_os_gpio_desc *desc)
{
    (void)desc;

    return -ENOTSUP;
}

static int32_t bladerf2_headless_gpio_get_direction(struct no_os_gpio_desc *desc,
                                                     uint8_t *direction)
{
    if (NULL == desc || NULL == direction) {
        return -EINVAL;
    }

    *direction = NO_OS_GPIO_OUT;

    return 0;
}

static int32_t bladerf2_headless_gpio_set_value(struct no_os_gpio_desc *desc,
                                                uint8_t value)
{
    uint32_t reg;
    uint32_t pin_mask;

    if (NULL == desc || desc->number < 0 || desc->number >= 32) {
        return -EINVAL;
    }

    pin_mask = UINT32_C(1) << desc->number;
    if (!rffe_csr_read_checked(&reg)) {
        return -EIO;
    }

    if (value) {
        reg |= pin_mask;
    } else {
        reg &= ~pin_mask;
    }

    if (!rffe_csr_write_checked(reg, pin_mask)) {
        return -EIO;
    }

    return 0;
}

static int32_t bladerf2_headless_gpio_get_value(struct no_os_gpio_desc *desc,
                                                uint8_t *value)
{
    uint32_t reg;

    if (NULL == desc || NULL == value ||
        desc->number < 0 || desc->number >= 32) {
        return -EINVAL;
    }

    if (!rffe_csr_read_checked(&reg)) {
        return -EIO;
    }

    *value = (reg & (UINT32_C(1) << desc->number)) ? 1 : 0;

    return 0;
}

const struct no_os_gpio_platform_ops bladerf2_gpio_ops = {
    .gpio_ops_get = bladerf2_headless_gpio_get,
    .gpio_ops_get_optional = bladerf2_headless_gpio_get_optional,
    .gpio_ops_remove = bladerf2_headless_gpio_remove,
    .gpio_ops_direction_input = bladerf2_headless_gpio_direction_input,
    .gpio_ops_direction_output = bladerf2_headless_gpio_direction_output,
    .gpio_ops_get_direction = bladerf2_headless_gpio_get_direction,
    .gpio_ops_set_value = bladerf2_headless_gpio_set_value,
    .gpio_ops_get_value = bladerf2_headless_gpio_get_value,
};
