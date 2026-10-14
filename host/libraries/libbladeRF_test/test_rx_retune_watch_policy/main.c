#include <assert.h>
#include <stdint.h>

#include "rf_transition_policy.h"

int main(void)
{
    const uint64_t rfdc_history[3] = {
        UINT64_C(2400000000), 0, 0,
    };
    bool watch_active = false;
    bool status_unavailable_reported = true;
    uint64_t generation = 4;

    bladerf2_rx_retune_watch_begin(
        &watch_active, &generation, &status_unavailable_reported);
    assert(watch_active && generation == 5);
    assert(!status_unavailable_reported);
    status_unavailable_reported = true;
    bladerf2_rx_retune_watch_retire(&watch_active, &generation);
    assert(!watch_active && generation == 6);
    bladerf2_rx_retune_watch_begin(
        &watch_active, &generation, &status_unavailable_reported);
    assert(watch_active && generation == 7);
    assert(!status_unavailable_reported);

    assert(bladerf2_rx_retune_watch_generation_current(true, 4, 4));
    assert(!bladerf2_rx_retune_watch_generation_current(true, 4, 5));
    assert(!bladerf2_rx_retune_watch_generation_current(false, 4, 4));
    assert(bladerf2_rx_retune_watch_observation(
               false, 1, 1, false, false, 0, 0) ==
           BLADERF2_RX_RETUNE_WATCH_NONE);
    assert(bladerf2_rx_retune_watch_observation(
               true, 1, 1, true, true, 7, 7) ==
           BLADERF2_RX_RETUNE_WATCH_NONE);
    assert(bladerf2_rx_retune_watch_observation(
               true, 1, 1, true, true, 7, 8) ==
           BLADERF2_RX_RETUNE_WATCH_FAILED);
    assert(bladerf2_rx_retune_watch_observation(
               true, 1, 1, true, true, 8, 7) ==
           BLADERF2_RX_RETUNE_WATCH_STATUS_UNAVAILABLE);
    assert(bladerf2_rx_retune_watch_observation(
               true, 1, 1, false, true, 7, 0) ==
           BLADERF2_RX_RETUNE_WATCH_STATUS_UNAVAILABLE);
    assert(bladerf2_rx_retune_watch_observation(
               true, 1, 1, true, false, 0, 9) ==
           BLADERF2_RX_RETUNE_WATCH_NONE);
    assert(bladerf2_rx_retune_watch_observation(
               true, 1, 2, true, true, 7, 8) ==
           BLADERF2_RX_RETUNE_WATCH_NONE);

    /* An epoch on a never-calibrated input needs RFDC even at the same LO;
     * same-port moves up to 100 MHz do not, larger moves do. */
    assert(bladerf2_rx_rfdc_calibration_required(
        rfdc_history, 1u << 0, 1, UINT64_C(2400000000)));
    assert(!bladerf2_rx_rfdc_calibration_required(
        rfdc_history, 1u << 0, 0, UINT64_C(2499999999)));
    assert(!bladerf2_rx_rfdc_calibration_required(
        rfdc_history, 1u << 0, 0, UINT64_C(2500000000)));
    assert(bladerf2_rx_rfdc_calibration_required(
        rfdc_history, 1u << 0, 0, UINT64_C(2500000001)));
    return 0;
}
