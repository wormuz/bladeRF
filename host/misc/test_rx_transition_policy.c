/* Requirement policy must never promise IQ validity without an FPGA epoch. */
#include <assert.h>
#include <stdint.h>

#include "rf_transition_policy.h"

int main(void)
{
    uint32_t effective = UINT32_MAX;
    struct bladerf_rf_event events[4] = {0};
    struct bladerf_rf_event final_event = {0};
    struct bladerf_rf_event invalidation = {
        .host_monotonic_ns = 99,
        .fpga_timestamp = 101,
        .transaction_id = 0,
        .epoch_id = 7,
        .requested_rx_lo_hz = 1840000000,
        .readback_rx_lo_hz = 1840000000,
        .rfic_status = 0x55,
        .fpga_state = BLADERF_RF_STATE_RX_DATA_INVALID,
        .event_type = BLADERF_RF_EVT_RX_DATA_INVALIDATED,
        .flags = BLADERF_RF_INVALIDATE_FEATURE,
        .error_code = BLADERF_ERR_UNEXPECTED,
    };
    struct bladerf_rf_event invalidation_channel;

    assert(bladerf2_rx_transition_channel_event_flags(
               BLADERF_CHANNEL_RX(0), true) ==
           BLADERF_RF_EVENT_F_TRANSITION_CHANNEL_VALID);
    assert(bladerf2_rx_transition_channel_event_flags(
               BLADERF_CHANNEL_RX(1), true) ==
           (BLADERF_RF_EVENT_F_TRANSITION_CHANNEL_VALID |
            BLADERF_RF_EVENT_F_TRANSITION_RX2));
    assert(bladerf2_rx_transition_channel_event_flags(
               BLADERF_CHANNEL_RX(1), false) == 0);
    assert(bladerf2_rx_transition_channel_event_flags(
               BLADERF_CHANNEL_TX(0), true) == 0);

    invalidation_channel = bladerf2_rx_invalidation_channel_event(
        &invalidation,
        BLADERF_RF_EVENT_F_TRANSITION_CHANNEL_VALID |
        BLADERF_RF_EVENT_F_TRANSITION_RX2 |
        BLADERF_RF_INVALIDATE_FEATURE);
    assert(invalidation_channel.event_type ==
           BLADERF_RF_EVT_RX_INVALIDATION_CHANNEL);
    assert(invalidation_channel.epoch_id == invalidation.epoch_id);
    assert(invalidation_channel.host_monotonic_ns ==
           invalidation.host_monotonic_ns);
    assert(invalidation_channel.fpga_timestamp == invalidation.fpga_timestamp);
    assert(invalidation_channel.flags ==
           (BLADERF_RF_EVENT_F_TRANSITION_CHANNEL_VALID |
            BLADERF_RF_EVENT_F_TRANSITION_RX2));
    assert(invalidation_channel.rfic_status == 0);
    assert(invalidation_channel.error_code == 0);
    assert(invalidation.flags == BLADERF_RF_INVALIDATE_FEATURE);

    /* A transition-time RFIC failure keeps its reason in the primary event;
     * channel provenance belongs only to the adjacent companion record. */
    invalidation.flags = BLADERF_RF_INVALIDATE_RFIC_BBPLL_UNLOCKED;
    invalidation_channel = bladerf2_rx_invalidation_channel_event(
        &invalidation,
        bladerf2_rx_transition_channel_event_flags(
            BLADERF_CHANNEL_RX(1), true));
    assert(invalidation.flags == BLADERF_RF_INVALIDATE_RFIC_BBPLL_UNLOCKED);
    assert(invalidation_channel.event_type ==
           BLADERF_RF_EVT_RX_INVALIDATION_CHANNEL);
    assert(invalidation_channel.flags ==
           (BLADERF_RF_EVENT_F_TRANSITION_CHANNEL_VALID |
            BLADERF_RF_EVENT_F_TRANSITION_RX2));
    assert(invalidation_channel.epoch_id == invalidation.epoch_id);
    assert(invalidation_channel.host_monotonic_ns ==
           invalidation.host_monotonic_ns);

    /* A status read which returns at or after the deadline is not a timely
     * completion, even if its sampled hardware bit is already asserted. */
    assert(!bladerf2_rf_transition_deadline_expired(99, 100));
    assert(bladerf2_rf_transition_deadline_expired(100, 100));
    assert(bladerf2_rf_transition_deadline_expired(101, 100));

    /* RF-link startup accepts only a new, fault-free generation ACK for the
     * requested direction. A stale high direction bit cannot certify START. */
    const uint32_t rf_link_version_mask = 0xf0000000u;
    const uint32_t rf_link_version = 0x10000000u;
    const uint32_t rf_link_epoch_count_mask = 0x0ff00000u;
    const uint32_t rf_link_tx_ack = 1u << 9;
    const uint32_t rf_link_tx_fault = 1u << 15;
    const uint32_t rf_link_entry = rf_link_version | (7u << 20) |
                                   rf_link_tx_ack;
    const uint32_t rf_link_new_ack = rf_link_version | (8u << 20) |
                                     rf_link_tx_ack;
    assert(bladerf2_rf_link_epoch_ack_valid(
        rf_link_entry, rf_link_new_ack, rf_link_version_mask,
        rf_link_version, rf_link_epoch_count_mask, rf_link_tx_ack,
        rf_link_tx_fault));
    assert(!bladerf2_rf_link_epoch_ack_valid(
        rf_link_entry, rf_link_entry, rf_link_version_mask,
        rf_link_version, rf_link_epoch_count_mask, rf_link_tx_ack,
        rf_link_tx_fault));
    assert(!bladerf2_rf_link_epoch_ack_valid(
        rf_link_entry, rf_link_version | (8u << 20), rf_link_version_mask,
        rf_link_version, rf_link_epoch_count_mask, rf_link_tx_ack,
        rf_link_tx_fault));
    assert(!bladerf2_rf_link_epoch_ack_valid(
        rf_link_entry, rf_link_new_ack | rf_link_tx_fault,
        rf_link_version_mask, rf_link_version, rf_link_epoch_count_mask,
        rf_link_tx_ack, rf_link_tx_fault));
    assert(!bladerf2_rf_link_epoch_ack_valid(
        rf_link_entry, 0x20000000u | (8u << 20) | rf_link_tx_ack,
        rf_link_version_mask, rf_link_version, rf_link_epoch_count_mask,
        rf_link_tx_ack, rf_link_tx_fault));
    const uint32_t rf_link_rx_ack = 1u << 8;
    const uint32_t rf_link_rx_fault = 1u << 14;
    const uint32_t rf_link_both_ack = rf_link_version | (9u << 20) |
                                     rf_link_tx_ack | rf_link_rx_ack;
    assert(bladerf2_rf_link_epoch_ack_valid(
        rf_link_version | (8u << 20) | rf_link_tx_ack | rf_link_rx_ack,
        rf_link_both_ack, rf_link_version_mask, rf_link_version,
        rf_link_epoch_count_mask, rf_link_tx_ack | rf_link_rx_ack,
        rf_link_tx_fault | rf_link_rx_fault));
    assert(!bladerf2_rf_link_epoch_ack_valid(
        rf_link_version | (8u << 20) | rf_link_tx_ack | rf_link_rx_ack,
        rf_link_version | (9u << 20) | rf_link_tx_ack,
        rf_link_version_mask, rf_link_version, rf_link_epoch_count_mask,
        rf_link_tx_ack | rf_link_rx_ack,
        rf_link_tx_fault | rf_link_rx_fault));
    assert(!bladerf2_rf_link_epoch_ack_valid(
        rf_link_version | (8u << 20) | rf_link_tx_ack | rf_link_rx_ack,
        rf_link_both_ack | rf_link_rx_fault, rf_link_version_mask,
        rf_link_version, rf_link_epoch_count_mask,
        rf_link_tx_ack | rf_link_rx_ack,
        rf_link_tx_fault | rf_link_rx_fault));
    assert(bladerf2_rf_link_epoch_id(rf_link_both_ack, 3) == 9);
    assert(bladerf2_rf_link_epoch_id(0x20000000u | (10u << 20), 3) == 3);

    /* Runtime TX validity requires the acknowledged epoch to remain active.
     * Readable status with an absent/stale epoch or stopped link fails closed. */
    const uint32_t tx_epoch_ok = 0x20000000u | (9u << 20) |
        BLADERF2_RF_LINK_STATUS_TX_LINK_ACTIVE |
        BLADERF2_RF_LINK_STATUS_TX_EPOCH_CURRENT |
        BLADERF2_RF_LINK_STATUS_TX_EPOCH_VALID;
    assert(bladerf2_rf_link_tx_epoch_valid(tx_epoch_ok, 9));
    assert(!bladerf2_rf_link_tx_epoch_valid(tx_epoch_ok, 8));
    assert(!bladerf2_rf_link_tx_epoch_valid(
        tx_epoch_ok & ~BLADERF2_RF_LINK_STATUS_TX_EPOCH_CURRENT, 9));
    assert(!bladerf2_rf_link_tx_epoch_valid(
        tx_epoch_ok & ~BLADERF2_RF_LINK_STATUS_TX_EPOCH_VALID, 9));
    assert(!bladerf2_rf_link_tx_epoch_valid(
        tx_epoch_ok & ~BLADERF2_RF_LINK_STATUS_TX_LINK_ACTIVE, 9));

    assert(bladerf2_rf_event_is_transition_terminal(
        BLADERF_RF_EVT_RX_EPOCH_VALID));
    assert(!bladerf2_rf_event_is_transition_terminal(
        BLADERF_RF_EVT_RX_FIRST_VALID_HOST_DATA));
    assert(!bladerf2_rf_event_is_transition_terminal(
        BLADERF_RF_EVT_RX_DATA_RESUMED));
    assert(!bladerf2_rf_event_is_transition_terminal(
        BLADERF_RF_EVT_CONFIG_ACCEPTED));
    assert(!bladerf2_rf_event_is_transition_terminal(
        BLADERF_RF_EVT_TX_LINK_VALID));
    assert(!bladerf2_rf_event_is_transition_terminal(
        BLADERF_RF_EVT_TX_UNDERRUN));
    assert(!bladerf2_rf_event_is_transition_terminal(
        BLADERF_RF_EVT_TX_LINK_FAULT));
    assert(!bladerf2_rf_event_is_transition_terminal(
        BLADERF_RF_EVT_TX_COUNTER_RESET));
    assert(!bladerf2_rf_event_is_transition_terminal(
        BLADERF_RF_EVT_TX_STATUS_RECOVERED));
    assert(!bladerf2_rf_event_is_transition_terminal(
        BLADERF_RF_EVT_TX_EPOCH_INVALID));

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

    assert(bladerf2_rf_transition_normalize_requirements(
        BLADERF_RF_REQUIRE_FIRST_HOST_DATA, false, &effective));
    assert(effective == (BLADERF_RF_REQUIRE_FIRST_HOST_DATA |
                         BLADERF_RF_REQUIRE_EPOCH_VALID |
                         BLADERF_RF_REQUIRE_PLL_LOCKED |
                         BLADERF_RF_REQUIRE_ENSM_RX |
                         BLADERF_RF_REQUIRE_BBPLL_LOCKED));

    assert(bladerf2_rf_transition_normalize_requirements(
        BLADERF_RF_REQUIRE_RX_X2_HOST_DATA, false, &effective));
    assert(effective == (BLADERF_RF_REQUIRE_RX_X2_HOST_DATA |
                         BLADERF_RF_REQUIRE_FIRST_HOST_DATA |
                         BLADERF_RF_REQUIRE_EPOCH_VALID |
                         BLADERF_RF_REQUIRE_PLL_LOCKED |
                         BLADERF_RF_REQUIRE_ENSM_RX |
                         BLADERF_RF_REQUIRE_BBPLL_LOCKED));

    /* Never silently accept a requirement the implementation cannot honor. */
    assert(!bladerf2_rf_transition_normalize_requirements(
        1U << 31, false, &effective));
    assert(!bladerf2_rf_transition_normalize_requirements(0, false, NULL));

    /* Ordinary scheduled retunes are permitted only when no RX transition
     * owns the channel. The transition's internal quick-tune substep is the
     * sole exception while a transaction is pending. */
    assert(!bladerf2_rx_scheduled_retune_blocked(
        false, true, false));
    assert(bladerf2_rx_scheduled_retune_blocked(
        true, true, false));
    assert(!bladerf2_rx_scheduled_retune_blocked(
        true, false, false));
    assert(!bladerf2_rx_scheduled_retune_blocked(
        true, true, true));
    assert(!bladerf2_rx_scheduled_retune_blocked(
        true, false, true));

    /* The public queue-clear API must obey the same RX epoch boundary: it
     * cannot cancel a fastlock request belonging to the active transaction.
     * The begin path retains one scoped internal cancellation exception. */
    assert(bladerf2_rx_scheduled_retune_cancel_blocked(
        true, true, true, true, false));
    assert(bladerf2_rx_scheduled_retune_cancel_blocked(
        true, false, true, false, false));
    assert(bladerf2_rx_scheduled_retune_cancel_blocked(
        true, false, false, true, false));
    assert(!bladerf2_rx_scheduled_retune_cancel_blocked(
        false, true, true, true, false));
    assert(!bladerf2_rx_scheduled_retune_cancel_blocked(
        true, true, true, true, true));
    assert(!bladerf2_rx_scheduled_retune_cancel_blocked(
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
    assert(bladerf2_rx_transition_next_transaction_id(0) == 1);
    assert(bladerf2_rx_transition_next_transaction_id(41) == 42);
    assert(bladerf2_rx_transition_next_transaction_id(UINT32_MAX) == 1);
    assert(bladerf2_rf_event_cursor_is_valid(0, 0));
    assert(bladerf2_rf_event_cursor_is_valid(99, 99));
    assert(bladerf2_rf_event_cursor_is_valid(98, 99));
    assert(!bladerf2_rf_event_cursor_is_valid(100, 99));
    assert(!bladerf2_rf_event_cursor_is_valid(UINT64_MAX, 0));
    assert(bladerf2_rx_first_host_data_admissible(false, false, 0, 0));
    assert(bladerf2_rx_first_host_data_admissible(true, true, 0, 0));
    assert(!bladerf2_rx_first_host_data_admissible(true, false, 0, 1));
    assert(bladerf2_rx_first_host_data_admissible(true, false, 100, 99));
    assert(!bladerf2_rx_first_host_data_admissible(true, false, 100, 100));
    assert(!bladerf2_rx_first_host_data_admissible(true, false, 100, 101));
    assert(!bladerf2_rx_first_host_data_admissible(true, false, 100, 0));
    assert(bladerf2_rx_first_host_data_before_deadline(99, 100));
    assert(!bladerf2_rx_first_host_data_before_deadline(100, 100));
    assert(!bladerf2_rx_first_host_data_before_deadline(101, 100));
    assert(!bladerf2_rx_first_host_data_before_deadline(0, 100));
    assert(!bladerf2_rx_first_host_data_before_deadline(99, 0));
    assert(bladerf2_rx_layout_satisfies_transition(false, BLADERF_RX_X1));
    assert(bladerf2_rx_layout_satisfies_transition(false, BLADERF_RX_X2));
    assert(!bladerf2_rx_layout_satisfies_transition(true, BLADERF_RX_X1));
    assert(bladerf2_rx_layout_matches_channel_mask(
        BLADERF_RX_X1, BLADERF_CHANNEL_RX(0), true, 0x1));
    assert(!bladerf2_rx_layout_matches_channel_mask(
        BLADERF_RX_X1, BLADERF_CHANNEL_RX(1), true, 0x1));
    assert(bladerf2_rx_layout_matches_channel_mask(
        BLADERF_RX_X1, BLADERF_CHANNEL_RX(1), true, 0x2));
    assert(!bladerf2_rx_layout_matches_channel_mask(
        BLADERF_RX_X1, BLADERF_CHANNEL_RX(1), false, 0x2));
    assert(bladerf2_rx_layout_matches_channel_mask(
        BLADERF_RX_X2, BLADERF_CHANNEL_RX(1), true, 0x3));
    assert(!bladerf2_rx_layout_matches_channel_mask(
        BLADERF_RX_X2, BLADERF_CHANNEL_RX(1), true, 0x1));
    assert(bladerf2_rx_layout_satisfies_transition(true, BLADERF_RX_X2));
    assert(!bladerf2_rx_x1_consumer_blocks_x2_transition(false, 1, true));
    assert(bladerf2_rx_x1_consumer_blocks_x2_transition(true, 1, false));
    assert(bladerf2_rx_x1_consumer_blocks_x2_transition(true, 0, true));
    assert(!bladerf2_rx_x1_consumer_blocks_x2_transition(true, 0, false));
    return 0;
}
