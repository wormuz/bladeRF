#include <assert.h>
#include <stdint.h>

#include "command_uart_fault_policy.h"

int main(void)
{
    struct command_uart_fault_state state = { false, 0 };

    /* A failed response in the current session fences that session. */
    assert(command_uart_fault_latch(&state, 7, 7));
    assert(state.latched && state.session_generation == 7);
    assert(!command_uart_fault_recover(&state, 7));

    /* Repeated failures cannot silently advance the recovery generation. */
    assert(command_uart_fault_latch(&state, 7, 7));
    assert(state.session_generation == 7);

    /* Session reset is the only recovery and rebinds the fault state. */
    assert(command_uart_fault_recover(&state, 8));
    assert(!state.latched && state.session_generation == 8);
    assert(!command_uart_fault_recover(&state, 8));

    /* A late failure from an old session must not poison the new one. */
    assert(!command_uart_fault_latch(&state, 7, 8));
    assert(!state.latched && state.session_generation == 8);

    /* Generation wrap is still a changed session unless a full 2^32
     * reconnects occurred between observations. */
    assert(command_uart_fault_latch(&state, UINT32_MAX, UINT32_MAX));
    assert(command_uart_fault_recover(&state, 0));
    assert(!state.latched && state.session_generation == 0);
    return 0;
}
