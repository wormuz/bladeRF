#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "command_uart_response_policy.h"

int main(void)
{
    bool prepared = false;
    unsigned int packet_actions = 0;

    if (command_uart_response_slot_result(0) !=
            COMMAND_UART_TRY_PUBLISH_OK ||
        command_uart_response_slot_result(
            COMMAND_UART_STATUS_RESPONSE_ACTIVE) !=
            COMMAND_UART_TRY_PUBLISH_OK ||
        command_uart_response_slot_result(
            COMMAND_UART_STATUS_RESPONSE_PENDING) !=
            COMMAND_UART_TRY_PUBLISH_BACKPRESSURE ||
        command_uart_response_slot_result(
            COMMAND_UART_STATUS_RESPONSE_PENDING |
            COMMAND_UART_STATUS_RESPONSE_OVERFLOW) !=
            COMMAND_UART_TRY_PUBLISH_ERROR) {
        fputs("UART response slot state was classified incorrectly\n", stderr);
        return 1;
    }

    if (command_uart_response_should_prepare(prepared)) {
        ++packet_actions;
        prepared = true;
    }
    if (command_uart_response_slot_result(
            COMMAND_UART_STATUS_RESPONSE_PENDING) !=
            COMMAND_UART_TRY_PUBLISH_BACKPRESSURE ||
        command_uart_response_should_prepare(prepared)) {
        fputs("backpressure would repeat the packet action\n", stderr);
        return 1;
    }

    if (command_uart_response_slot_result(0) !=
            COMMAND_UART_TRY_PUBLISH_OK || packet_actions != 1) {
        fputs("released response slot did not preserve exactly-once work\n",
              stderr);
        return 1;
    }

    prepared = false;
    if (!command_uart_response_should_prepare(prepared)) {
        fputs("retired response state blocked the next request\n", stderr);
        return 1;
    }

    return 0;
}
