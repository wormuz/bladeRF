/* Requirement policy must never promise IQ validity without an FPGA epoch. */
#include <assert.h>
#include <stdint.h>

#include "rf_transition_policy.h"

int main(void)
{
    uint32_t effective = UINT32_MAX;
    struct bladerf_rf_event events[4] = {0};
    struct bladerf_rf_event final_event = {0};

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
     * event transaction owns the channel or epoch protection is active
     * (sync filter or async META contract); only the transaction's own
     * quick-tune substep is allowed. */
    assert(!bladerf2_rx_scheduled_retune_blocked(
        false, true, true, true, false));
    assert(bladerf2_rx_scheduled_retune_blocked(
        true, true, false, false, false));
    assert(bladerf2_rx_scheduled_retune_blocked(
        true, false, false, true, false));
    /* Async META users have no sync filter object, but the enabled RX epoch
     * contract must still block this unreported retune. */
    assert(bladerf2_rx_scheduled_retune_blocked(
        true, false, true, false, false));
    assert(!bladerf2_rx_scheduled_retune_blocked(
        true, true, true, true, true));
    assert(!bladerf2_rx_scheduled_retune_blocked(
        true, false, false, false, false));

    /* A runtime notification can follow the terminal transition event.
     * Waiting must select the latest event for that transaction rather than
     * an unrelated overrun with transaction_id zero. */
    events[0].transaction_id = 42;
    events[0].event_type = BLADERF_RF_EVT_CONFIG_ACCEPTED;
    events[1].transaction_id = 42;
    events[1].event_type = BLADERF_RF_EVT_RX_EPOCH_VALID;
    events[2].transaction_id = 0;
    events[2].event_type = BLADERF_RF_EVT_RX_STREAM_OVERRUN;
    assert(bladerf2_rf_event_latest_for_transaction(
        events, 4, 3, 3, 42, &final_event));
    assert(final_event.transaction_id == 42);
    assert(final_event.event_type == BLADERF_RF_EVT_RX_EPOCH_VALID);

    /* Reverse traversal remains correct after the ring wraps. */
    events[0].transaction_id = 7;
    events[0].event_type = BLADERF_RF_EVT_ERROR;
    events[3].transaction_id = 7;
    events[3].event_type = BLADERF_RF_EVT_CONTROL_PLANE_CONFIRMED;
    assert(bladerf2_rf_event_latest_for_transaction(
        events, 4, 1, 3, 7, &final_event));
    assert(final_event.event_type == BLADERF_RF_EVT_ERROR);
    assert(!bladerf2_rf_event_latest_for_transaction(
        events, 4, 1, 3, 99, &final_event));
    return 0;
}
