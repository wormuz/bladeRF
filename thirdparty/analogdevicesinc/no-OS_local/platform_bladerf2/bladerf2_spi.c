/*
 * This file is part of the bladeRF project:
 *   http://www.github.com/nuand/bladeRF
 *
 * Copyright (C) 2026 Nuand LLC
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/*
 * SPI back end for the AD9361 driver, as a no-OS platform ops table.
 *
 * The older driver revision this replaces had no such table: it called
 * spi_write_then_read() directly, and the bladeRF tree carried a patch that
 * substituted its own spi_read()/spi_write() inside the driver body behind
 * #ifdef NUAND_MODIFICATIONS. The current driver takes the transfer through
 * struct no_os_spi_platform_ops, so the substitution belongs here instead and
 * the driver source stays unmodified.
 *
 * Buffer layout produced by the driver (see ad9361_spi_read(),
 * ad9361_spi_write() and ad9361_spi_writem()):
 *
 *   buf[0] = cmd >> 8      cmd = AD_READ or AD_WRITE, | AD_CNT(n) | AD_ADDR(reg)
 *   buf[1] = cmd & 0xFF
 *   buf[2..] = payload, n bytes, read into or written from
 *
 * The bladeRF back end takes the 16-bit command and up to 8 payload bytes
 * packed into a 64-bit word, most significant byte first, so this file only
 * has to split the buffer and pack or unpack the payload.
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "board/board.h"
#include "backend/usb/nios_access.h"
#include "log.h"

#include "no_os_delay.h"
#include "no_os_mutex.h"
#include "no_os_spi.h"

/* Payload bytes one transaction can carry. Mirrors MAX_MBYTE_SPI, which the
 * driver defines as 8 in ad9361.h; kept local so this file does not need the
 * driver's private header. */
#define BLADERF2_SPI_MAX_PAYLOAD 8

/* Command word bit that marks a write. ad9361.h defines AD_READ as (0 << 15)
 * and AD_WRITE as (1 << 15), so bit 15 is the direction. */
#define BLADERF2_SPI_CMD_WRITE (1u << 15)

enum gain_table_batch_state {
    GAIN_TABLE_BATCH_UNKNOWN,
    GAIN_TABLE_BATCH_SUPPORTED,
    GAIN_TABLE_BATCH_UNSUPPORTED,
};

struct bladerf2_spi_context {
    struct bladerf *dev;
    enum gain_table_batch_state gain_table_batch;
};

static int32_t bladerf2_spi_init(struct no_os_spi_desc **desc,
                                 const struct no_os_spi_init_param *param)
{
    struct no_os_spi_desc *d;
    struct bladerf2_spi_context *ctx;

    if (NULL == desc || NULL == param) {
        return -EINVAL;
    }

    d = calloc(1, sizeof(*d));
    if (NULL == d) {
        return -ENOMEM;
    }

    ctx = calloc(1, sizeof(*ctx));
    if (NULL == ctx) {
        free(d);
        return -ENOMEM;
    }

    /* The bladeRF device handle travels in extra, set by the caller that
     * builds the init parameters. Everything else in the descriptor is
     * unused: the transfer goes through the device back end, not a bus
     * peripheral this file owns. */
    ctx->dev = param->extra;
    {
        const char *disable_batch = getenv("BLADERF_DISABLE_GAIN_TABLE_BATCH");
        if (disable_batch != NULL && disable_batch[0] != '\0' &&
            strcmp(disable_batch, "0") != 0) {
            ctx->gain_table_batch = GAIN_TABLE_BATCH_UNSUPPORTED;
        }
    }
    d->extra = ctx;
    d->platform_ops = param->platform_ops;

    *desc = d;

    return 0;
}

static int32_t bladerf2_spi_remove(struct no_os_spi_desc *desc)
{
    if (desc != NULL) {
        free(desc->extra);
    }
    free(desc);

    return 0;
}

static int32_t bladerf2_spi_write_and_read(struct no_os_spi_desc *desc,
                                           uint8_t *data,
                                           uint16_t bytes_number)
{
    struct bladerf2_spi_context *ctx;
    struct bladerf *dev;
    uint16_t cmd;
    uint16_t payload;
    uint64_t word;
    uint16_t i;
    int status;

    if (NULL == desc || NULL == data) {
        return -EINVAL;
    }

    ctx = desc->extra;
    if (ctx == NULL) {
        return -EINVAL;
    }

    /* Two command bytes are mandatory; anything shorter is not a transfer
     * this driver produces. */
    if (bytes_number < 2) {
        return -EINVAL;
    }

    payload = (uint16_t)(bytes_number - 2);
    if (payload > BLADERF2_SPI_MAX_PAYLOAD) {
        return -EINVAL;
    }

    dev = ctx->dev;
    if (NULL == dev) {
        return -EINVAL;
    }

    cmd = (uint16_t)((((uint16_t)data[0]) << 8) | data[1]);

    if (cmd & BLADERF2_SPI_CMD_WRITE) {
        /* Pack the payload most significant byte first, matching what the
         * back end expects to shift onto the bus. */
        word = 0;
        for (i = 0; i < payload; i++) {
            word |= ((uint64_t)data[2 + i]) << (8 * (7 - i));
        }

        status = dev->backend->ad9361_spi_write(dev, cmd, word);
        if (status < 0) {
            return -EIO;
        }

        return 0;
    }

    word = 0;
    status = dev->backend->ad9361_spi_read(dev, cmd, &word);
    if (status < 0) {
        return -EIO;
    }

    /* The read replaces the payload in place; the command bytes are left
     * alone, as the driver only looks past them. */
    for (i = 0; i < payload; i++) {
        data[2 + i] = (uint8_t)((word >> (8 * (7 - i))) & 0xff);
    }

    return 0;
}

static int32_t bladerf2_spi_write_gain_table_row(struct no_os_spi_desc *desc,
                                                  uint16_t row,
                                                  uint8_t data1,
                                                  uint8_t data2,
                                                  uint8_t data3,
                                                  uint8_t config,
                                                  uint32_t delay_us)
{
    struct bladerf2_spi_context *ctx;
    struct bladerf *dev;
    int status;

    if (desc == NULL || desc->extra == NULL || desc->bus == NULL) {
        return -EINVAL;
    }

    ctx = desc->extra;
    dev = ctx->dev;
    no_os_mutex_lock(desc->bus->mutex);

    if (ctx->gain_table_batch != GAIN_TABLE_BATCH_UNSUPPORTED) {
        status = nios_ad9361_gain_table_row(dev, row, data1, data2, data3,
                                            config, delay_us);
        if (status == 0) {
            ctx->gain_table_batch = GAIN_TABLE_BATCH_SUPPORTED;
            no_os_mutex_unlock(desc->bus->mutex);
            return 0;
        }

        /* Old Nios images reject this packet target. Disable batching for
         * this device session and replay the same row as ordinary ordered
         * SPI writes so FPGA loading and legacy images keep working. A
         * transport failure after partial execution is also safe: writing
         * the same indexed table row again is idempotent. */
        ctx->gain_table_batch = GAIN_TABLE_BATCH_UNSUPPORTED;
        log_debug("Gain-table row batching unavailable (%s); using SPI fallback\n",
                  bladerf_strerror(status));
    }

    static const uint16_t regs[] = { 0x130, 0x131, 0x132, 0x133, 0x137 };
    const uint8_t values[] = {
        (uint8_t)row, data1, data2, data3, config
    };
    for (size_t i = 0; i < sizeof(regs) / sizeof(regs[0]); ++i) {
        uint8_t buf[3] = {
            (uint8_t)(BLADERF2_SPI_CMD_WRITE | ((regs[i] >> 8) & 0x7f)),
            (uint8_t)regs[i],
            values[i],
        };
        status = bladerf2_spi_write_and_read(desc, buf, sizeof(buf));
        if (status < 0) {
            no_os_mutex_unlock(desc->bus->mutex);
            return status;
        }
    }

    no_os_udelay(delay_us);
    no_os_mutex_unlock(desc->bus->mutex);
    return 0;
}

const struct no_os_spi_platform_ops bladerf2_spi_ops = {
    .init = bladerf2_spi_init,
    .write_and_read = bladerf2_spi_write_and_read,
    .write_gain_table_row = bladerf2_spi_write_gain_table_row,
    .remove = bladerf2_spi_remove,
};
