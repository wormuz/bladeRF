/* Requirement policy must never promise IQ validity without an FPGA epoch. */
#include <assert.h>
#include <stdint.h>

#include "channel_policy.h"
#include "rf_transition_policy.h"
#include "../libraries/libbladeRF/src/board/bladerf2/rfic_rssi_policy.h"

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
    uint64_t loss_count = 0;
    struct bladerf2_rx_invalidation_policy invalidation_policy;

    assert(bladerf2_rfic_frequency_model_valid(true, true));
    assert(!bladerf2_rfic_frequency_model_valid(false, true));
    assert(!bladerf2_rfic_frequency_model_valid(true, false));
    assert(!bladerf2_rfic_frequency_model_valid(false, false));

    assert(bladerf_public_channel_valid(BLADERF_CHANNEL_RX(0)));
    assert(bladerf_public_channel_valid(BLADERF_CHANNEL_RX(1)));
    assert(bladerf_public_channel_valid(BLADERF_CHANNEL_TX(0)));
    assert(bladerf_public_channel_valid(BLADERF_CHANNEL_TX(1)));
    assert(!bladerf_public_channel_valid(BLADERF_CHANNEL_RX(2)));
    assert(!bladerf_public_channel_valid(BLADERF_CHANNEL_TX(2)));
    assert(!bladerf_public_channel_valid(BLADERF_CHANNEL_INVALID));

    assert(bladerf2_clock_select_valid(CLOCK_SELECT_ONBOARD));
    assert(bladerf2_clock_select_valid(CLOCK_SELECT_EXTERNAL));
    assert(!bladerf2_clock_select_valid((bladerf_clock_select)2));
    assert(bladerf2_channel_valid(BLADERF_CHANNEL_RX(0)));
    assert(bladerf2_channel_valid(BLADERF_CHANNEL_RX(1)));
    assert(bladerf2_channel_valid(BLADERF_CHANNEL_TX(0)));
    assert(bladerf2_channel_valid(BLADERF_CHANNEL_TX(1)));
    assert(!bladerf2_channel_valid(BLADERF_CHANNEL_RX(2)));
    assert(!bladerf2_channel_valid(BLADERF_CHANNEL_INVALID));
    assert(bladerf2_rssi_channel_known(BLADERF_CHANNEL_RX(0)));
    assert(bladerf2_rssi_channel_known(BLADERF_CHANNEL_TX(1)));
    assert(!bladerf2_rssi_channel_known(BLADERF_CHANNEL_RX(2)));
    assert(bladerf2_rssi_channel_supported(BLADERF_CHANNEL_RX(1)));
    assert(!bladerf2_rssi_channel_supported(BLADERF_CHANNEL_TX(0)));
    assert(bladerf2_rssi_multiplier_valid(1));
    assert(!bladerf2_rssi_multiplier_valid(0));
    assert(!bladerf2_rssi_multiplier_valid(-1));
    assert(bladerf2_rx_channel_valid(BLADERF_CHANNEL_RX(0)));
    assert(bladerf2_rx_channel_valid(BLADERF_CHANNEL_RX(1)));
    assert(!bladerf2_rx_channel_valid(BLADERF_CHANNEL_TX(0)));
    assert(!bladerf2_rx_channel_valid(BLADERF_CHANNEL_RX(2)));
    assert(!bladerf2_rx_channel_valid(BLADERF_CHANNEL_INVALID));

    invalidation_policy = bladerf2_rx_invalidation_policy(
        BLADERF_CHANNEL_TX(0), false);
    assert(invalidation_policy.reserve_setter);
    assert(!invalidation_policy.abort_rx_epoch);
    invalidation_policy = bladerf2_rx_invalidation_policy(
        BLADERF_CHANNEL_TX(0), true);
    assert(invalidation_policy.reserve_setter);
    assert(invalidation_policy.abort_rx_epoch);
    invalidation_policy = bladerf2_rx_invalidation_policy(
        BLADERF_CHANNEL_RX(1), false);
    assert(invalidation_policy.reserve_setter);
    assert(invalidation_policy.abort_rx_epoch);

    assert(bladerf2_loss_counter_snapshot(7, 0x12345678, 7, &loss_count));
    assert(loss_count == 0x0000000712345678ULL);
    assert(!bladerf2_loss_counter_snapshot(
        7, 0xffffffff, 8, &loss_count));
    assert(loss_count == 0x0000000712345678ULL);

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

    /* RX FIR configuration uses AD9361 rx=3 and invalidates the shared RX
     * epoch. Its event provenance must follow the active epoch lane. */
    assert(bladerf2_rx_invalidation_reason_is_shared(
        BLADERF_RF_INVALIDATE_RX_FIR));
    assert(bladerf2_rx_invalidation_reason_is_shared(
        BLADERF_RF_INVALIDATE_RFIC_REG));
    assert(!bladerf2_rx_invalidation_reason_is_shared(
        BLADERF_RF_INVALIDATE_GAIN));

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
    const uint32_t rf_link_tx_active = 1u << 0;
    const uint32_t rf_link_tx_fault = 1u << 15;
    const uint32_t rf_link_entry = rf_link_version | (7u << 20) |
                                   rf_link_tx_ack;
    const uint32_t rf_link_new_ack = rf_link_version | (8u << 20) |
                                     rf_link_tx_ack | rf_link_tx_active;
    assert(bladerf2_rf_link_epoch_ack_valid(
        rf_link_entry, rf_link_new_ack, rf_link_version_mask,
        rf_link_version, rf_link_epoch_count_mask, rf_link_tx_ack,
        rf_link_tx_active, rf_link_tx_fault));
    assert(!bladerf2_rf_link_epoch_ack_valid(
        rf_link_entry, rf_link_entry, rf_link_version_mask,
        rf_link_version, rf_link_epoch_count_mask, rf_link_tx_ack,
        rf_link_tx_active, rf_link_tx_fault));
    assert(!bladerf2_rf_link_epoch_ack_valid(
        rf_link_entry, rf_link_version | (8u << 20), rf_link_version_mask,
        rf_link_version, rf_link_epoch_count_mask, rf_link_tx_ack,
        rf_link_tx_active, rf_link_tx_fault));
    assert(!bladerf2_rf_link_epoch_ack_valid(
        rf_link_entry, rf_link_version | (8u << 20) | rf_link_tx_ack,
        rf_link_version_mask, rf_link_version, rf_link_epoch_count_mask,
        rf_link_tx_ack, rf_link_tx_active, rf_link_tx_fault));
    assert(!bladerf2_rf_link_epoch_ack_valid(
        rf_link_entry, rf_link_new_ack | rf_link_tx_fault,
        rf_link_version_mask, rf_link_version, rf_link_epoch_count_mask,
        rf_link_tx_ack, rf_link_tx_active, rf_link_tx_fault));
    assert(!bladerf2_rf_link_epoch_ack_valid(
        rf_link_entry, 0x20000000u | (8u << 20) | rf_link_tx_ack,
        rf_link_version_mask, rf_link_version, rf_link_epoch_count_mask,
        rf_link_tx_ack, rf_link_tx_active, rf_link_tx_fault));
    const uint32_t rf_link_rx_ack = 1u << 8;
    const uint32_t rf_link_rx_active = 1u << 16;
    const uint32_t rf_link_rx_fault = 1u << 14;
    const uint32_t rf_link_both_ack = rf_link_version | (9u << 20) |
                                     rf_link_tx_ack | rf_link_rx_ack |
                                     rf_link_tx_active | rf_link_rx_active;
    assert(bladerf2_rf_link_epoch_ack_valid(
        rf_link_version | (8u << 20) | rf_link_tx_ack | rf_link_rx_ack,
        rf_link_both_ack, rf_link_version_mask, rf_link_version,
        rf_link_epoch_count_mask, rf_link_tx_ack | rf_link_rx_ack,
        rf_link_tx_active | rf_link_rx_active,
        rf_link_tx_fault | rf_link_rx_fault));
    assert(!bladerf2_rf_link_epoch_ack_valid(
        rf_link_version | (8u << 20) | rf_link_tx_ack | rf_link_rx_ack,
        rf_link_version | (9u << 20) | rf_link_tx_ack,
        rf_link_version_mask, rf_link_version, rf_link_epoch_count_mask,
        rf_link_tx_ack | rf_link_rx_ack,
        rf_link_tx_active | rf_link_rx_active,
        rf_link_tx_fault | rf_link_rx_fault));
    assert(!bladerf2_rf_link_epoch_ack_valid(
        rf_link_version | (8u << 20) | rf_link_tx_ack | rf_link_rx_ack,
        rf_link_both_ack & ~rf_link_rx_active, rf_link_version_mask,
        rf_link_version, rf_link_epoch_count_mask,
        rf_link_tx_ack | rf_link_rx_ack,
        rf_link_tx_active | rf_link_rx_active,
        rf_link_tx_fault | rf_link_rx_fault));
    assert(!bladerf2_rf_link_epoch_ack_valid(
        rf_link_version | (8u << 20) | rf_link_tx_ack | rf_link_rx_ack,
        rf_link_both_ack | rf_link_rx_fault, rf_link_version_mask,
        rf_link_version, rf_link_epoch_count_mask,
        rf_link_tx_ack | rf_link_rx_ack,
        rf_link_tx_active | rf_link_rx_active,
        rf_link_tx_fault | rf_link_rx_fault));
    assert(bladerf2_rf_link_epoch_id(rf_link_both_ack, 3) == 9);
    assert(bladerf2_rf_link_epoch_id(0x20000000u | (10u << 20), 3) == 3);
    assert(bladerf2_rf_link_stop_required(true, false, false));
    assert(!bladerf2_rf_link_stop_required(false, false, false));
    assert(!bladerf2_rf_link_stop_required(true, true, false));
    assert(!bladerf2_rf_link_stop_required(true, false, true));
    assert(bladerf2_rf_link_stop_failure_actionable(
        false, BLADERF_ERR_FPGA_OP, BLADERF_ERR_FPGA_OP));
    assert(!bladerf2_rf_link_stop_failure_actionable(
        true, BLADERF_ERR_FPGA_OP, BLADERF_ERR_FPGA_OP));
    assert(bladerf2_rf_link_stop_failure_actionable(
        true, BLADERF_ERR_TIMEOUT, BLADERF_ERR_FPGA_OP));
    /* A preceding status-read timeout leaves legacy support unknown, so even
     * FPGA_OP from STOP must be surfaced instead of treated as compatibility. */
    assert(bladerf2_rf_link_stop_failure_actionable(
        false, BLADERF_ERR_FPGA_OP, BLADERF_ERR_FPGA_OP));
    assert(!bladerf2_rf_link_stop_failure_actionable(
        false, 0, BLADERF_ERR_FPGA_OP));

    /* Runtime TX validity and STATUS_RECOVERED require the acknowledged epoch
     * to remain active. Readable status with an absent/stale epoch or stopped
     * link fails closed. */
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
    assert(bladerf2_tx_rfic_observation(
        true, BLADERF2_AD9361_VCO_LOCK_BIT,
        BLADERF2_AD9361_ENSM_STATE_TX,
        BLADERF2_AD9361_BBPLL_LOCK_BIT) ==
           BLADERF2_TX_RFIC_OBSERVATION_VALID);
    assert(bladerf2_tx_rfic_observation(
        true, BLADERF2_AD9361_VCO_LOCK_BIT,
        BLADERF2_AD9361_ENSM_STATE_FDD,
        BLADERF2_AD9361_BBPLL_LOCK_BIT) ==
           BLADERF2_TX_RFIC_OBSERVATION_VALID);
    assert(bladerf2_tx_rfic_observation(
        false, BLADERF2_AD9361_VCO_LOCK_BIT,
        BLADERF2_AD9361_ENSM_STATE_TX,
        BLADERF2_AD9361_BBPLL_LOCK_BIT) ==
           BLADERF2_TX_RFIC_OBSERVATION_UNAVAILABLE);
    /* An FPGA TX fault prevents the monitor from reading these registers;
     * their zero-initialized storage is unavailable evidence, not proof of
     * RFPLL/ENSM/BBPLL invalidity. */
    assert(bladerf2_tx_rfic_observation(false, 0, 0, 0) ==
           BLADERF2_TX_RFIC_OBSERVATION_UNAVAILABLE);
    assert(bladerf2_tx_rfic_observation(
        true, 0, BLADERF2_AD9361_ENSM_STATE_TX,
        BLADERF2_AD9361_BBPLL_LOCK_BIT) ==
           BLADERF2_TX_RFIC_OBSERVATION_INVALID);
    assert(bladerf2_tx_rfic_observation(
        true, BLADERF2_AD9361_VCO_LOCK_BIT, 0x08,
        BLADERF2_AD9361_BBPLL_LOCK_BIT) ==
           BLADERF2_TX_RFIC_OBSERVATION_INVALID);
    assert(bladerf2_tx_rfic_observation(
        true, BLADERF2_AD9361_VCO_LOCK_BIT,
        BLADERF2_AD9361_ENSM_STATE_TX, 0) ==
           BLADERF2_TX_RFIC_OBSERVATION_INVALID);

    assert(bladerf2_retune_completion_batch_valid(0, 0, 16));
    assert(bladerf2_retune_completion_batch_valid(16, 16, 16));
    assert(!bladerf2_retune_completion_batch_valid(17, 17, 16));
    assert(!bladerf2_retune_completion_batch_valid(2, 1, 16));
    assert(bladerf2_retune_completion_matches(
        1234, 1840000000ULL, 3, 1234, 1840000000ULL, 3));
    assert(!bladerf2_retune_completion_matches(
        1234, 1840000000ULL, 3, 1235, 1840000000ULL, 3));
    assert(!bladerf2_retune_completion_matches(
        1234, 1840000000ULL, 3, 1234, 1840000001ULL, 3));
    assert(!bladerf2_retune_completion_matches(
        1234, 1840000000ULL, 3, 1234, 1840000000ULL, 4));

    /* A TX error arriving during a shared START must not be overwritten by
     * the subsequently observed link acknowledgement. */
    assert(bladerf2_tx_epoch_generation_unchanged(12, 12));
    assert(!bladerf2_tx_epoch_generation_unchanged(13, 12));

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
    assert(bladerf2_tx_admission_allowed(
        true, false, false, false, 0, false, true));
    assert(!bladerf2_tx_admission_allowed(
        true, false, false, true, 0, false, true));
    assert(!bladerf2_tx_admission_allowed(
        true, false, true, false, 0, false, true));
    assert(!bladerf2_tx_admission_allowed(
        true, true, false, false, 0, false, true));
    assert(!bladerf2_tx_admission_allowed(
        true, false, false, false, 1, false, true));
    assert(!bladerf2_tx_admission_allowed(
        true, false, false, false, 0, true, true));
    assert(!bladerf2_tx_admission_allowed(
        true, false, false, false, 0, false, false));
    assert(!bladerf2_tx_admission_allowed(
        false, false, false, false, 0, false, true));
    return 0;
}
