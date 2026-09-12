/*
 * SPI back end for the AD9361 driver, as a no-OS platform ops table, for the
 * Nios softcore build.
 *
 * Mirrors bladerf2_spi.c (host variant), which adapts the new
 * no_os_spi_platform_ops shape to the old spi_write()/spi_read() calling
 * convention the driver used to make directly. On Nios the underlying
 * transfer is adi_spi_write()/adi_spi_read() in devices.c (common
 * bladeRF_nios app code) rather than a USB backend call - the cmd+payload
 * packing the driver produces is identical on both platforms (see
 * bladerf2_spi.c for the buffer layout description), only the transport
 * differs.
 */
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>

#include "devices.h"

#include "no_os_spi.h"

/* Payload bytes one transaction can carry. Mirrors MAX_MBYTE_SPI, which the
 * driver defines as 8 in ad9361.h; kept local so this file does not need the
 * driver's private header. */
#define BLADERF2_SPI_MAX_PAYLOAD 8

/* Command word bit that marks a write. ad9361.h defines AD_READ as (0 << 15)
 * and AD_WRITE as (1 << 15), so bit 15 is the direction. */
#define BLADERF2_SPI_CMD_WRITE (1u << 15)

static int32_t bladerf2_headless_spi_init(struct no_os_spi_desc **desc,
                                          const struct no_os_spi_init_param *param)
{
    struct no_os_spi_desc *d;

    if (NULL == desc || NULL == param) {
        return -EINVAL;
    }

    d = calloc(1, sizeof(*d));
    if (NULL == d) {
        return -ENOMEM;
    }

    d->extra = param->extra;
    d->platform_ops = param->platform_ops;

    *desc = d;

    return 0;
}

static int32_t bladerf2_headless_spi_remove(struct no_os_spi_desc *desc)
{
    free(desc);

    return 0;
}

static int32_t bladerf2_headless_spi_write_and_read(struct no_os_spi_desc *desc,
                                                     uint8_t *data,
                                                     uint16_t bytes_number)
{
    uint16_t cmd;
    uint16_t payload;
    uint64_t word;
    uint16_t i;

    if (NULL == desc || NULL == data) {
        return -EINVAL;
    }

    if (bytes_number < 2) {
        return -EINVAL;
    }

    payload = (uint16_t)(bytes_number - 2);
    if (payload > BLADERF2_SPI_MAX_PAYLOAD) {
        return -EINVAL;
    }

    cmd = (uint16_t)((((uint16_t)data[0]) << 8) | data[1]);

    if (cmd & BLADERF2_SPI_CMD_WRITE) {
        word = 0;
        for (i = 0; i < payload; i++) {
            word |= ((uint64_t)data[2 + i]) << (8 * (7 - i));
        }

        adi_spi_write(cmd, word);

        return 0;
    }

    word = adi_spi_read(cmd);

    for (i = 0; i < payload; i++) {
        data[2 + i] = (uint8_t)((word >> (8 * (7 - i))) & 0xff);
    }

    return 0;
}

const struct no_os_spi_platform_ops bladerf2_spi_ops = {
    .init = bladerf2_headless_spi_init,
    .write_and_read = bladerf2_headless_spi_write_and_read,
    .remove = bladerf2_headless_spi_remove,
};
