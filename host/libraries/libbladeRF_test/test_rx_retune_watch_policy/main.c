#include <assert.h>
#include <stdint.h>

#include "rf_transition_policy.h"

int main(void)
{
    assert(bladerf2_rx_retune_watch_observation(
               false, false, false, 0, 0) ==
           BLADERF2_RX_RETUNE_WATCH_NONE);
    assert(bladerf2_rx_retune_watch_observation(
               true, true, true, 7, 7) ==
           BLADERF2_RX_RETUNE_WATCH_NONE);
    assert(bladerf2_rx_retune_watch_observation(
               true, true, true, 7, 8) ==
           BLADERF2_RX_RETUNE_WATCH_FAILED);
    assert(bladerf2_rx_retune_watch_observation(
               true, true, true, 8, 7) ==
           BLADERF2_RX_RETUNE_WATCH_STATUS_UNAVAILABLE);
    assert(bladerf2_rx_retune_watch_observation(
               true, false, true, 7, 0) ==
           BLADERF2_RX_RETUNE_WATCH_STATUS_UNAVAILABLE);
    assert(bladerf2_rx_retune_watch_observation(
               true, true, false, 0, 9) ==
           BLADERF2_RX_RETUNE_WATCH_NONE);
    return 0;
}
