/* This file is part of the bladeRF project:
 *   http://www.github.com/nuand/bladeRF
 *
 * Copyright (c) 2026 Nuand LLC
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#ifndef COMMAND_UART_RESPONSE_MMIO_H_
#define COMMAND_UART_RESPONSE_MMIO_H_

#include <stdint.h>

#include "command_uart_response_policy.h"

#define COMMAND_UART_RESPONSE_SESSION_OFFSET 24u
#define COMMAND_UART_RESPONSE_STATUS_OFFSET 28u

struct command_uart_response_mmio {
    uint32_t (*read32)(void *context, uint32_t offset);
    void (*write32)(void *context, uint32_t offset, uint32_t value);
    void *context;
};

static inline enum command_uart_try_publish_result
command_uart_response_mmio_try_publish(
    const struct command_uart_response_mmio *mmio, const uint8_t *response,
    uint32_t session_generation)
{
    uint32_t current_generation;
    uint32_t status;
    uint32_t word;
    unsigned int i;

    current_generation = mmio->read32(
        mmio->context, COMMAND_UART_RESPONSE_SESSION_OFFSET);
    if (session_generation != current_generation) {
        return COMMAND_UART_TRY_PUBLISH_STALE;
    }

    status = mmio->read32(mmio->context, COMMAND_UART_RESPONSE_STATUS_OFFSET);
    if (command_uart_response_slot_result(status) !=
        COMMAND_UART_TRY_PUBLISH_OK) {
        return command_uart_response_slot_result(status);
    }

    /* The session tag and three payload words are staged first. Writing the
     * final word raises command_in in the FPGA and commits the full frame. */
    mmio->write32(mmio->context, COMMAND_UART_RESPONSE_SESSION_OFFSET,
                  session_generation);
    for (i = 0; i < 16; i += 4) {
        word = ((uint32_t)response[i]) |
               (((uint32_t)response[i + 1]) << 8) |
               (((uint32_t)response[i + 2]) << 16) |
               (((uint32_t)response[i + 3]) << 24);
        mmio->write32(mmio->context, i, word);
    }

    return COMMAND_UART_TRY_PUBLISH_OK;
}

#endif
