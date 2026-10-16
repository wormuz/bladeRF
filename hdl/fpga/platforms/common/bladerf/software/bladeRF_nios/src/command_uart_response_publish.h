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

#ifndef BLADERF_NIOS_COMMAND_UART_RESPONSE_PUBLISH_H_
#define BLADERF_NIOS_COMMAND_UART_RESPONSE_PUBLISH_H_

#include <stdbool.h>
#include <stdint.h>

#include "command_uart_fault_policy.h"
#include "command_uart_response_policy.h"

struct command_uart_response_publish_ops {
    uint32_t (*read_session_generation)(void *context);
    enum command_uart_try_publish_result (*try_write_response)(
        void *context, uint8_t *response, uint32_t session_generation);
    void (*set_isr_enabled)(void *context, bool enabled);
    void *context;
};

static inline enum command_uart_try_publish_result
command_uart_response_publish_try(
    const struct command_uart_response_publish_ops *ops,
    struct command_uart_fault_state *fault, uint8_t *response,
    uint32_t session_generation)
{
    enum command_uart_try_publish_result result;
    uint32_t current_generation;

    current_generation = ops->read_session_generation(ops->context);
    if (session_generation != current_generation) {
        return COMMAND_UART_TRY_PUBLISH_STALE;
    }

    result = ops->try_write_response(
        ops->context, response, session_generation);
    if (result == COMMAND_UART_TRY_PUBLISH_ERROR) {
        current_generation = ops->read_session_generation(ops->context);
        if (session_generation != current_generation) {
            return COMMAND_UART_TRY_PUBLISH_STALE;
        }
        if (command_uart_fault_latch(fault, session_generation,
                                     current_generation)) {
            ops->set_isr_enabled(ops->context, false);
        }
    }

    return result;
}

static inline bool command_uart_response_publish_recover(
    const struct command_uart_response_publish_ops *ops,
    struct command_uart_fault_state *fault, uint32_t current_generation)
{
    if (!command_uart_fault_recover(fault, current_generation)) {
        return false;
    }

    ops->set_isr_enabled(ops->context, true);
    return true;
}

#endif
