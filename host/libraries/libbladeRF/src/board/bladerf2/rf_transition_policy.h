/* Event requirement normalization for the bladeRF 2 RX transition API. */
#ifndef BLADERF2_RF_TRANSITION_POLICY_H_
#define BLADERF2_RF_TRANSITION_POLICY_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <libbladeRF.h>

/* Preserve the transition handle that scoped an RX epoch on invalid-data
 * notifications. The epoch is shared by both RX lanes, but its requested
 * control channel is useful provenance for RX1/RX2 consumers. */
static inline uint32_t bladerf2_rx_transition_channel_event_flags(
    bladerf_channel channel, bool channel_valid)
{
    if (!channel_valid ||
        (channel != BLADERF_CHANNEL_RX(0) &&
         channel != BLADERF_CHANNEL_RX(1))) {
        return 0;
    }

    return BLADERF_RF_EVENT_F_TRANSITION_CHANNEL_VALID |
           (channel == BLADERF_CHANNEL_RX(1)
                ? BLADERF_RF_EVENT_F_TRANSITION_RX2 : 0);
}

/* A hardware observation is timely only if it completed strictly before the
 * caller's deadline. Checking only before a blocking SPI/NIOS read can accept
 * a late completion and turn an expired wait into success. */
static inline bool bladerf2_rf_transition_deadline_expired(
    uint64_t now_ns, uint64_t deadline_ns)
{
    return now_ns >= deadline_ns;
}

static inline bool bladerf2_rf_transition_normalize_requirements(
    uint32_t requested, bool require_rx_data_valid, uint32_t *effective)
{
    const uint32_t supported = BLADERF_RF_REQUIRE_PLL_LOCKED |
                              BLADERF_RF_REQUIRE_ENSM_RX |
                              BLADERF_RF_REQUIRE_DATAPATH_ARMED |
                              BLADERF_RF_REQUIRE_EPOCH_VALID |
                              BLADERF_RF_REQUIRE_BBPLL_LOCKED |
                              BLADERF_RF_REQUIRE_FIRST_HOST_DATA |
                              BLADERF_RF_REQUIRE_RX_X2_HOST_DATA;

    if (effective == NULL || (requested & ~supported) != 0) {
        return false;
    }

    *effective = requested;
    if (require_rx_data_valid ||
        (requested & BLADERF_RF_REQUIRE_FIRST_HOST_DATA) != 0 ||
        (requested & BLADERF_RF_REQUIRE_RX_X2_HOST_DATA) != 0 ||
        (requested & BLADERF_RF_REQUIRE_DATAPATH_ARMED) != 0) {
        *effective |= BLADERF_RF_REQUIRE_EPOCH_VALID;
    }

    if (*effective == 0) {
        return false;
    }

    if (*effective & BLADERF_RF_REQUIRE_RX_X2_HOST_DATA) {
        *effective |= BLADERF_RF_REQUIRE_FIRST_HOST_DATA;
    }

    if ((*effective & BLADERF_RF_REQUIRE_EPOCH_VALID) != 0) {
        *effective |= BLADERF_RF_REQUIRE_PLL_LOCKED |
                      BLADERF_RF_REQUIRE_ENSM_RX |
                      BLADERF_RF_REQUIRE_BBPLL_LOCKED;
    }
    return true;
}

static inline bool bladerf2_rf_event_is_transition_terminal(
    bladerf_rf_event_type type)
{
    return type == BLADERF_RF_EVT_RX_EPOCH_VALID ||
           type == BLADERF_RF_EVT_CONTROL_PLANE_CONFIRMED ||
           type == BLADERF_RF_EVT_RX_DATAPATH_ARMED ||
           type == BLADERF_RF_EVT_RX_EPOCH_ABORT_FAILED ||
           type == BLADERF_RF_EVT_ERROR;
}

/* Runtime RX notifications share the history ring with transition events.
 * Host-data lifecycle events carry the epoch's transaction ID, but happen
 * after transition completion and must not replace the event returned by
 * rx_transition_wait(). If ring pressure has also evicted the terminal
 * transition event, fail closed instead of returning CONFIG_ACCEPTED as
 * successful completion. */
static inline bool bladerf2_rf_event_latest_transition_result_for_transaction(
    const struct bladerf_rf_event *events, uint32_t capacity, uint32_t head,
    uint32_t count, uint32_t transaction_id,
    struct bladerf_rf_event *result)
{
    if (events == NULL || result == NULL || capacity == 0 ||
        head >= capacity || count > capacity || transaction_id == 0) {
        return false;
    }

    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t slot = (head + capacity - 1u - i) % capacity;
        if (events[slot].transaction_id == transaction_id) {
            if (!bladerf2_rf_event_is_transition_terminal(
                    events[slot].event_type)) {
                continue;
            }
            *result = events[slot];
            return true;
        }
    }

    return false;
}

/* An independently queued RX recall has no transaction completion event or
 * epoch boundary. Only the fastlock request issued from inside the active
 * transition may pass while the event-driven contract owns the channel. */
static inline bool bladerf2_rx_scheduled_retune_blocked(
    bool is_rx, bool transition_pending, bool epoch_contract_enabled,
    bool sync_epoch_filter_enabled, bool transaction_fastlock)
{
    return is_rx && !transaction_fastlock &&
           (transition_pending || epoch_contract_enabled ||
            sync_epoch_filter_enabled);
}

/* Epoch-required transitions cannot safely hand data to a running async RX
 * consumer that has no packet-level epoch identity. */
static inline bool bladerf2_rx_epoch_transition_blocked_by_async_format(
    bool require_epoch, unsigned int epochless_async_stream_count)
{
    return require_epoch && epochless_async_stream_count != 0;
}

/* Runtime status reads happen outside the async admission lock. Match their
 * result to both the short FPGA epoch tag and the host transaction that was
 * certified when the read began, so an old observation cannot revoke a later
 * epoch after the 8-bit FPGA epoch counter wraps. */
static inline bool bladerf2_rx_fault_observation_is_current(
    uint32_t observed_transaction_id, uint32_t current_transaction_id,
    uint8_t observed_epoch_id, uint8_t current_epoch_id)
{
    return observed_transaction_id != 0 &&
           observed_transaction_id == current_transaction_id &&
           observed_epoch_id == current_epoch_id;
}

/* Transaction ID zero is reserved by the public history APIs. Keep it
 * reserved when the 32-bit sequence wraps during long-running sweeps. */
static inline uint32_t bladerf2_rx_transition_next_transaction_id(
    uint32_t previous_id)
{
    return previous_id == UINT32_MAX ? 1u : previous_id + 1u;
}

/* A cursor beyond the current tail cannot be treated as an empty, complete
 * history: doing so would silently hide every event until the ring catches
 * up to that fabricated sequence. */
static inline bool bladerf2_rf_event_cursor_is_valid(uint64_t after_sequence,
                                                     uint64_t current_sequence)
{
    return after_sequence <= current_sequence;
}

static inline bool bladerf2_rx_first_host_data_admissible(
    bool required, bool already_reported, uint64_t deadline_ns,
    uint64_t now_ns)
{
    return !required || already_reported ||
           (deadline_ns != 0 && now_ns != 0 && now_ns < deadline_ns);
}

static inline bool bladerf2_rx_first_host_data_before_deadline(
    uint64_t event_ns, uint64_t deadline_ns)
{
    return event_ns != 0 && deadline_ns != 0 && event_ns < deadline_ns;
}

static inline bool bladerf2_rx_layout_satisfies_transition(
    bool require_rx_x2, bladerf_channel_layout layout)
{
    return !require_rx_x2 || layout == BLADERF_RX_X2;
}

/* RX_X1 has no channel identity in its layout enum, so its host-data event
 * may certify only the single RFIC lane selected by the transition. RX_X2
 * requires both channel enables. `mask` uses bit 0 = RX1, bit 1 = RX2. */
static inline bool bladerf2_rx_layout_matches_channel_mask(
    bladerf_channel_layout layout, bladerf_channel transition_channel,
    bool mask_valid, uint8_t mask)
{
    if (!mask_valid) {
        return false;
    }
    if (layout == BLADERF_RX_X2) {
        return mask == 0x3;
    }
    if (layout != BLADERF_RX_X1 || BLADERF_CHANNEL_IS_TX(transition_channel) ||
        ((unsigned int)transition_channel >> 1) > 1) {
        return false;
    }
    return mask == (uint8_t)(1u <<
                             ((unsigned int)transition_channel >> 1));
}

enum bladerf2_rx_channel_mask_observation {
    BLADERF2_RX_CHANNEL_MASK_UNAVAILABLE = 0,
    BLADERF2_RX_CHANNEL_MASK_MATCH,
    BLADERF2_RX_CHANNEL_MASK_CHANGED,
};

/* A certified RX epoch depends on the channel set remaining observable and
 * unchanged. Keep this decision in the shared policy layer so the runtime
 * monitor and native tests exercise the same fail-closed classification. */
static inline enum bladerf2_rx_channel_mask_observation
bladerf2_rx_channel_mask_observation(bool status_read_succeeded,
                                     bool expected_mask_valid,
                                     uint8_t expected_mask,
                                     uint8_t observed_mask)
{
    if (!status_read_succeeded || !expected_mask_valid) {
        return BLADERF2_RX_CHANNEL_MASK_UNAVAILABLE;
    }
    return expected_mask == observed_mask
        ? BLADERF2_RX_CHANNEL_MASK_MATCH
        : BLADERF2_RX_CHANNEL_MASK_CHANGED;
}

static inline bool bladerf2_rx_x1_consumer_blocks_x2_transition(
    bool require_rx_x2, unsigned int active_async_rx_x1,
    bool sync_rx_x1_configured)
{
    return require_rx_x2 &&
           (active_async_rx_x1 != 0 || sync_rx_x1_configured);
}

#endif
