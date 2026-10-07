/* Event requirement normalization for the bladeRF 2 RX transition API. */
#ifndef BLADERF2_RF_TRANSITION_POLICY_H_
#define BLADERF2_RF_TRANSITION_POLICY_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <libbladeRF.h>

static inline bool bladerf2_rf_transition_normalize_requirements(
    uint32_t requested, bool require_rx_data_valid, uint32_t *effective)
{
    const uint32_t supported = BLADERF_RF_REQUIRE_PLL_LOCKED |
                              BLADERF_RF_REQUIRE_ENSM_RX |
                              BLADERF_RF_REQUIRE_DATAPATH_ARMED |
                              BLADERF_RF_REQUIRE_EPOCH_VALID |
                              BLADERF_RF_REQUIRE_BBPLL_LOCKED;

    if (effective == NULL || (requested & ~supported) != 0) {
        return false;
    }

    *effective = requested;
    if (require_rx_data_valid ||
        (requested & BLADERF_RF_REQUIRE_DATAPATH_ARMED) != 0) {
        *effective |= BLADERF_RF_REQUIRE_EPOCH_VALID;
    }

    if (*effective == 0) {
        return false;
    }

    if ((*effective & BLADERF_RF_REQUIRE_EPOCH_VALID) != 0) {
        *effective |= BLADERF_RF_REQUIRE_PLL_LOCKED |
                      BLADERF_RF_REQUIRE_ENSM_RX |
                      BLADERF_RF_REQUIRE_BBPLL_LOCKED;
    }
    return true;
}

/* Runtime RX notifications share the history ring with transition events.
 * Host-data lifecycle events carry the epoch's transaction ID, but happen
 * after transition completion and must not replace the event returned by
 * rx_transition_wait(). */
static inline bool bladerf2_rf_event_latest_for_transaction(
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
            if (events[slot].event_type ==
                    BLADERF_RF_EVT_RX_FIRST_VALID_HOST_DATA ||
                events[slot].event_type == BLADERF_RF_EVT_RX_DATA_RESUMED) {
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

#endif
