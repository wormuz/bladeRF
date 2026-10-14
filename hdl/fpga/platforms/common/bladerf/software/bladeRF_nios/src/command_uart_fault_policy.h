/* This file is part of the bladeRF project:
 *   http://www.github.com/nuand/bladeRF
 *
 * Copyright (c) 2026 Nuand LLC
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 */

#ifndef BLADERF_NIOS_COMMAND_UART_FAULT_POLICY_H_
#define BLADERF_NIOS_COMMAND_UART_FAULT_POLICY_H_

#include <stdbool.h>
#include <stdint.h>

struct command_uart_fault_state {
    bool latched;
    uint32_t session_generation;
};

/* A stale response belongs to a session that is already gone; it must not
 * poison the replacement session. */
static inline bool command_uart_fault_latch(
    struct command_uart_fault_state *state, uint32_t response_generation,
    uint32_t current_generation)
{
    if (response_generation != current_generation) {
        return false;
    }

    state->latched = true;
    state->session_generation = current_generation;
    return true;
}

/* Hardware clears its sticky response-overflow state on session reset. */
static inline bool command_uart_fault_recover(
    struct command_uart_fault_state *state, uint32_t current_generation)
{
    if (!state->latched ||
        current_generation == state->session_generation) {
        return false;
    }

    state->latched = false;
    state->session_generation = current_generation;
    return true;
}

#endif
