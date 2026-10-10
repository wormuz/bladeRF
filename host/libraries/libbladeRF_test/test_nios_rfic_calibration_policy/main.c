#include <assert.h>
#include <stdint.h>

#include "rfic_calibration_policy.h"

#define RFDC_THRESHOLD_HZ UINT64_C(100000000)

int main(void)
{
    uint64_t prior_phy_calibration_hz = UINT64_C(1835000000);
    const uint64_t reinit_frequency_hz = UINT64_C(1835000000);

    assert(!rfic_rfdc_calibration_required(
        prior_phy_calibration_hz, reinit_frequency_hz, RFDC_THRESHOLD_HZ));

    /* OFF -> ON creates a new PHY, so old history must not suppress the
     * first calibration even when the requested LO is unchanged. */
    rfic_rfdc_calibration_history_reset(&prior_phy_calibration_hz);
    assert(prior_phy_calibration_hz == 0);
    assert(rfic_rfdc_calibration_required(
        prior_phy_calibration_hz, reinit_frequency_hz, RFDC_THRESHOLD_HZ));

    assert(!rfic_rfdc_calibration_required(
        reinit_frequency_hz, reinit_frequency_hz + RFDC_THRESHOLD_HZ - 1,
        RFDC_THRESHOLD_HZ));
    assert(rfic_rfdc_calibration_required(
        reinit_frequency_hz, reinit_frequency_hz + RFDC_THRESHOLD_HZ,
        RFDC_THRESHOLD_HZ));
    return 0;
}
