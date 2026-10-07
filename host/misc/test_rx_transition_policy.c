/* Requirement policy must never promise IQ validity without an FPGA epoch. */
#include <assert.h>
#include <stdint.h>

#include "rf_transition_policy.h"

int main(void)
{
    uint32_t effective = UINT32_MAX;
    struct bladerf_rf_event events[4] = {0};
    struct bladerf_rf_event final_event = {0};

    assert(bladerf2_rf_event_is_transition_terminal(
        BLADERF_RF_EVT_RX_EPOCH_VALID));
    assert(!bladerf2_rf_event_is_transition_terminal(
        BLADERF_RF_EVT_RX_FIRST_VALID_HOST_DATA));
    assert(!bladerf2_rf_event_is_transition_terminal(
        BLADERF_RF_EVT_RX_DATA_RESUMED));
    assert(!bladerf2_rf_event_is_transition_terminal(
        BLADERF_RF_EVT_CONFIG_ACCEPTED));

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

    /* Host-data lifecycle notifications keep the epoch transaction ID. They
     * may race the waiter's final history read but must not replace its
     * transition-completion result. */
    events[0].transaction_id = 42;
    events[0].event_type = BLADERF_RF_EVT_CONFIG_ACCEPTED;
    events[1].transaction_id = 42;
    events[1].event_type = BLADERF_RF_EVT_RX_EPOCH_VALID;
    events[2].transaction_id = 42;
    events[2].event_type = BLADERF_RF_EVT_RX_FIRST_VALID_HOST_DATA;
    events[3].transaction_id = 42;
    events[3].event_type = BLADERF_RF_EVT_RX_DATA_RESUMED;
    assert(bladerf2_rf_event_latest_transition_result_for_transaction(
        events, 4, 0, 4, 42, &final_event));
    assert(final_event.transaction_id == 42);
    assert(final_event.event_type == BLADERF_RF_EVT_RX_EPOCH_VALID);

    /* If ring pressure evicts every transition-completion event, lifecycle
     * notices and CONFIG_ACCEPTED are not enough to claim success. */
    events[1].event_type = BLADERF_RF_EVT_CONFIG_ACCEPTED;
    assert(!bladerf2_rf_event_latest_transition_result_for_transaction(
        events, 4, 0, 4, 42, &final_event));

    /* Reverse traversal remains correct after the ring wraps. */
    events[0].transaction_id = 7;
    events[0].event_type = BLADERF_RF_EVT_ERROR;
    events[3].transaction_id = 7;
    events[3].event_type = BLADERF_RF_EVT_CONTROL_PLANE_CONFIRMED;
    assert(bladerf2_rf_event_latest_transition_result_for_transaction(
        events, 4, 1, 3, 7, &final_event));
    assert(final_event.event_type == BLADERF_RF_EVT_ERROR);
    assert(!bladerf2_rf_event_latest_transition_result_for_transaction(
        events, 4, 1, 3, 99, &final_event));

    assert(bladerf2_rx_fault_observation_is_current(91, 91, 7, 7));
    assert(!bladerf2_rx_fault_observation_is_current(90, 91, 7, 7));
    /* The short FPGA epoch tag may wrap; the host transaction still fences
     * a delayed monitor observation from an earlier certification. */
    assert(!bladerf2_rx_fault_observation_is_current(1, 257, 7, 7));
    assert(!bladerf2_rx_fault_observation_is_current(0, 0, 7, 7));
    return 0;
}
