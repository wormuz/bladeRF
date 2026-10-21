#include <assert.h>
#include <stdint.h>

#include "rf_transition_policy.h"

/* NIOS latches an unknown RFIC state by clearing init_state to OFF while raw
 * AD9361 reads keep succeeding: the monitor must reject every state but ON,
 * and a failed INIT read. */
int main(void)
{
    assert(bladerf2_rx_monitor_nios_state_readable(0, true));
    assert(!bladerf2_rx_monitor_nios_state_readable(0, false));
    assert(!bladerf2_rx_monitor_nios_state_readable(BLADERF_ERR_UNEXPECTED,
                                                    true));
    return 0;
}
