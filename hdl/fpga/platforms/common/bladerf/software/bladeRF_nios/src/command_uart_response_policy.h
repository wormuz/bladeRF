/* Copyright (c) 2026 Nuand LLC */

#ifndef BLADERF_NIOS_COMMAND_UART_RESPONSE_POLICY_H_
#define BLADERF_NIOS_COMMAND_UART_RESPONSE_POLICY_H_

#include <stdbool.h>
#include <stdint.h>

#define COMMAND_UART_STATUS_RESPONSE_PENDING  (1u << 2)
#define COMMAND_UART_STATUS_RESPONSE_OVERFLOW (1u << 3)
#define COMMAND_UART_STATUS_RESPONSE_ACTIVE   (1u << 4)

enum command_uart_try_publish_result {
    COMMAND_UART_TRY_PUBLISH_ERROR = -1,
    COMMAND_UART_TRY_PUBLISH_BACKPRESSURE = 0,
    COMMAND_UART_TRY_PUBLISH_OK = 1,
    COMMAND_UART_TRY_PUBLISH_STALE = 2,
};

static inline enum command_uart_try_publish_result
command_uart_response_slot_result(uint32_t status)
{
    if (status & COMMAND_UART_STATUS_RESPONSE_OVERFLOW) {
        return COMMAND_UART_TRY_PUBLISH_ERROR;
    }
    if (status & COMMAND_UART_STATUS_RESPONSE_PENDING) {
        return COMMAND_UART_TRY_PUBLISH_BACKPRESSURE;
    }
    return COMMAND_UART_TRY_PUBLISH_OK;
}

/* A prepared response is immutable: retries must not execute its request. */
static inline bool command_uart_response_should_prepare(bool prepared)
{
    return !prepared;
}

/* The main-loop pump may retry only a response that already owns the packet
 * slot. An unprepared packet still needs the RFIC-safe-point dispatch path. */
static inline bool command_uart_response_should_retry(bool ready,
						      bool prepared)
{
	return ready && prepared;
}

static inline void command_uart_response_mark_prepared(volatile bool *prepared)
{
    *prepared = true;
}

static inline void command_uart_response_retire(volatile bool *prepared)
{
    *prepared = false;
}

#endif
