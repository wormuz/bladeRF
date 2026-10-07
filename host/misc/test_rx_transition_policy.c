/* Requirement policy must never promise IQ validity without an FPGA epoch. */
#include <assert.h>
#include <stdint.h>

#include "rf_transition_policy.h"

int main(void)
{
    uint32_t effective = UINT32_MAX;

    /* Empty requests are rejected; a control-only completion must still
     * identify at least one observed hardware condition. */
    assert(!bladerf2_rf_transition_normalize_requirements(0, false,
                                                           &effective));
    assert(bladerf2_rf_transition_normalize_requirements(
        BLADERF_RF_REQUIRE_PLL_LOCKED, false, &effective));
    assert(effective == BLADERF_RF_REQUIRE_PLL_LOCKED);
    assert(bladerf2_rf_transition_normalize_requirements(
        BLADERF_RF_REQUIRE_BBPLL_LOCKED, false, &effective));
    assert(effective == BLADERF_RF_REQUIRE_BBPLL_LOCKED);

    /* The data-valid boolean automatically requires the sample epoch and
     * RFPLL, BBPLL, and ENSM prerequisites, even if omitted from the mask. */
    assert(bladerf2_rf_transition_normalize_requirements(
        BLADERF_RF_REQUIRE_PLL_LOCKED, true, &effective));
    assert(effective == (BLADERF_RF_REQUIRE_PLL_LOCKED |
                         BLADERF_RF_REQUIRE_ENSM_RX |
                         BLADERF_RF_REQUIRE_EPOCH_VALID |
                         BLADERF_RF_REQUIRE_BBPLL_LOCKED));

    /* The older datapath-armed request is implemented by the stronger,
     * sample-backed epoch event rather than an unobserved software claim. */
    assert(bladerf2_rf_transition_normalize_requirements(
        BLADERF_RF_REQUIRE_DATAPATH_ARMED, false, &effective));
    assert(effective == (BLADERF_RF_REQUIRE_DATAPATH_ARMED |
                         BLADERF_RF_REQUIRE_PLL_LOCKED |
                         BLADERF_RF_REQUIRE_ENSM_RX |
                         BLADERF_RF_REQUIRE_EPOCH_VALID |
                         BLADERF_RF_REQUIRE_BBPLL_LOCKED));

    assert(bladerf2_rf_transition_normalize_requirements(
        BLADERF_RF_REQUIRE_EPOCH_VALID, false, &effective));
    assert((effective & (BLADERF_RF_REQUIRE_PLL_LOCKED |
                         BLADERF_RF_REQUIRE_ENSM_RX |
                         BLADERF_RF_REQUIRE_BBPLL_LOCKED)) ==
           (BLADERF_RF_REQUIRE_PLL_LOCKED | BLADERF_RF_REQUIRE_ENSM_RX |
            BLADERF_RF_REQUIRE_BBPLL_LOCKED));

    /* Never silently accept a requirement the implementation cannot honor. */
    assert(!bladerf2_rf_transition_normalize_requirements(
        1U << 31, false, &effective));
    assert(!bladerf2_rf_transition_normalize_requirements(0, false, NULL));

    /* A queued RX recall has no epoch completion. It is blocked whenever an
     * event transaction owns the channel or the host epoch filter is active;
     * only the quick-tune substep inside that transaction is allowed. */
    assert(!bladerf2_rx_scheduled_retune_blocked(
        false, true, true, false));
    assert(bladerf2_rx_scheduled_retune_blocked(
        true, true, false, false));
    assert(bladerf2_rx_scheduled_retune_blocked(
        true, false, true, false));
    assert(!bladerf2_rx_scheduled_retune_blocked(
        true, true, true, true));
    assert(!bladerf2_rx_scheduled_retune_blocked(
        true, false, false, false));
    return 0;
}
