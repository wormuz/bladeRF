/* Copyright 2026 Nuand LLC
 *
 * This file is part of the bladeRF project and is distributed under the
 * terms of the GNU Lesser General Public License, version 2.1 or later.
 */
#include <time.h>
#include <string.h>

#include <libbladeRF.h>

#include "bladeRF.h"
#include "board/board.h"
#include "common.h"
#include "rf_transition_policy.h"
#include "streaming/metadata.h"

static uint64_t monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

void bladerf2_rx_transition_fail_first_host_data_locked(
    struct bladerf2_board_data *board_data, int status)
{
    if (board_data == NULL || status == 0 ||
        !board_data->rf_transition_first_host_data_required ||
        board_data->rf_transition_first_host_data_reported ||
        board_data->rf_transition_first_host_data_failure != 0) {
        return;
    }

    board_data->rf_transition_first_host_data_failure = status;
    COND_SIGNAL(&board_data->rx_async_epoch_cond);
}

int bladerf2_rx_transition_host_revoke(struct bladerf *dev,
                                      struct bladerf_sync *sync,
                                      int transition_status)
{
    struct bladerf2_board_data *board_data;

    if (dev == NULL || dev->board_data == NULL || sync == NULL) {
        return BLADERF_ERR_INVAL;
    }
    board_data = dev->board_data;

    /* This is the software admission fence for every failed transition.
     * Revoke it before asking NIOS to ABORT: failed control I/O must never
     * leave the host parser's last successful epoch trusted. */
    MUTEX_LOCK(&board_data->rx_async_epoch_lock);
    board_data->rf_transition_epoch_certified = false;
    board_data->rx_async_have_expected_timestamp = false;
    board_data->rf_transition_first_host_data_reported = false;
    if (board_data->rf_transition_first_host_data_required &&
        board_data->rf_transition_first_host_data_failure == 0) {
        board_data->rf_transition_first_host_data_failure =
            transition_status != 0 ? transition_status : BLADERF_ERR_UNEXPECTED;
    }
    COND_SIGNAL(&board_data->rx_async_epoch_cond);
    MUTEX_UNLOCK(&board_data->rx_async_epoch_lock);

    /* The generation changes before sync->lock is acquired, so reads already
     * in flight also fail closed when this waits behind their parser lock. */
    return sync_rx_epoch_invalidate(sync);
}

static int rx_epoch_admission_deadline_status(uint64_t deadline_ns)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return BLADERF_ERR_UNEXPECTED;
    }
    const uint64_t now_ns = (uint64_t)ts.tv_sec * 1000000000ULL +
                            (uint64_t)ts.tv_nsec;
    return now_ns >= deadline_ns ? BLADERF_ERR_TIMEOUT : 0;
}

int bladerf2_rx_epoch_admission_prepare(
    struct bladerf2_board_data *board_data,
    const struct bladerf_rf_event *epoch_event, uint64_t deadline_ns,
    bool *admission_lock_held)
{
    int status;

    if (board_data == NULL || epoch_event == NULL ||
        admission_lock_held == NULL ||
        epoch_event->event_type != BLADERF_RF_EVT_RX_EPOCH_VALID ||
        epoch_event->fpga_state != BLADERF_RF_STATE_RX_DATA_VALID) {
        return BLADERF_ERR_INVAL;
    }
    *admission_lock_held = false;
    status = rx_epoch_admission_deadline_status(deadline_ns);
    if (status != 0) {
        return status;
    }

    MUTEX_LOCK(&board_data->rx_async_epoch_lock);
    *admission_lock_held = true;
    status = rx_epoch_admission_deadline_status(deadline_ns);
    if (status == 0 && !board_data->rf_transition_epoch_contract_enabled) {
        status = BLADERF_ERR_UNEXPECTED;
    }
    if (status == 0) {
        board_data->rf_transition_epoch_certified = true;
        board_data->rf_transition_certified_epoch_id =
            (uint8_t)epoch_event->epoch_id;
        board_data->rf_transition_first_valid_timestamp =
            epoch_event->fpga_timestamp;
        board_data->rf_transition_certified_epoch_event = *epoch_event;
        board_data->rf_transition_first_host_data_reported = false;
        board_data->rx_async_have_expected_timestamp = false;
    } else {
        *admission_lock_held = false;
        MUTEX_UNLOCK(&board_data->rx_async_epoch_lock);
    }
    return status;
}

void bladerf2_rx_epoch_admission_finish(
    struct bladerf2_board_data *board_data, bool *admission_lock_held)
{
    if (board_data != NULL && admission_lock_held != NULL &&
        *admission_lock_held) {
        *admission_lock_held = false;
        MUTEX_UNLOCK(&board_data->rx_async_epoch_lock);
    }
}

void bladerf2_rf_event_append_locked(
    struct bladerf2_board_data *board_data,
    const struct bladerf_rf_event *event)
{
    board_data->rf_transition_events[board_data->rf_transition_event_head] =
        *event;
    board_data->rf_transition_event_sequence++;
    board_data->rf_transition_event_sequences[
        board_data->rf_transition_event_head] =
            board_data->rf_transition_event_sequence;
    board_data->rf_transition_event_head =
        (board_data->rf_transition_event_head + 1) %
        BLADERF2_RF_EVENT_HISTORY_SIZE;
    if (board_data->rf_transition_event_count <
        BLADERF2_RF_EVENT_HISTORY_SIZE) {
        board_data->rf_transition_event_count++;
    }
}

void bladerf2_rf_event_append(struct bladerf2_board_data *board_data,
                              const struct bladerf_rf_event *event)
{
    MUTEX_LOCK(&board_data->rf_transition_event_lock);
    bladerf2_rf_event_append_locked(board_data, event);
    MUTEX_UNLOCK(&board_data->rf_transition_event_lock);
}

void bladerf2_rf_event_append_rx_invalidation_locked(
    struct bladerf2_board_data *board_data,
    const struct bladerf_rf_event *event, uint32_t channel_flags)
{
    struct bladerf_rf_event channel_event;
    const uint32_t channel_mask =
        BLADERF_RF_EVENT_F_TRANSITION_CHANNEL_VALID |
        BLADERF_RF_EVENT_F_TRANSITION_RX2;

    /* Both entries must be committed in one event-ring critical section. */
    if (board_data == NULL || event == NULL ||
        event->event_type != BLADERF_RF_EVT_RX_DATA_INVALIDATED) {
        return;
    }

    bladerf2_rf_event_append_locked(board_data, event);

    channel_flags &= channel_mask;
    if (channel_flags & BLADERF_RF_EVENT_F_TRANSITION_CHANNEL_VALID) {
        channel_event = bladerf2_rx_invalidation_channel_event(
            event, channel_flags);
        bladerf2_rf_event_append_locked(board_data, &channel_event);
    }
}

void bladerf2_rf_event_append_rx_invalidation(
    struct bladerf2_board_data *board_data,
    const struct bladerf_rf_event *event, uint32_t channel_flags)
{
    if (board_data == NULL) {
        return;
    }

    MUTEX_LOCK(&board_data->rf_transition_event_lock);
    bladerf2_rf_event_append_rx_invalidation_locked(
        board_data, event, channel_flags);
    MUTEX_UNLOCK(&board_data->rf_transition_event_lock);
}

void bladerf2_rx_format_unsupported(struct bladerf *dev,
                                    bladerf_format format,
                                    bool deduplicate)
{
    struct bladerf2_board_data *board_data;
    struct bladerf_rf_event event = {0};
    uint8_t epoch_id;
    uint32_t transition_channel_flags = 0;
    bool should_report = false;

    if (dev == NULL || dev->board_data == NULL) {
        return;
    }
    board_data = dev->board_data;

    /* May run in a USB callback or on a synchronous format preflight. Do not
     * take dev->lock: a setter may hold it while waiting for USB progress. */
    MUTEX_LOCK(&board_data->rx_async_epoch_lock);
    if (board_data->rf_transition_epoch_contract_enabled &&
        (!deduplicate || !board_data->rx_format_unsupported_reported)) {
        bladerf2_rx_transition_fail_first_host_data_locked(
            board_data, BLADERF_ERR_UNSUPPORTED);
        if (deduplicate) {
            board_data->rx_format_unsupported_reported = true;
            /* Repeated async packets are one withheld interval. A sync
             * config rejection is a separate configuration event and must
             * not suppress RX_DATA_WITHHELD if the caller then reads while
             * the epoch is still invalid. */
            board_data->rx_async_data_withheld_active = true;
        }
        should_report = true;
    }
    epoch_id = board_data->rf_transition_epoch_id;
    transition_channel_flags =
        bladerf2_rx_transition_channel_event_flags(
            board_data->rf_transition_current_channel,
            board_data->rf_transition_epoch_contract_enabled);
    MUTEX_UNLOCK(&board_data->rx_async_epoch_lock);

    if (!should_report) {
        return;
    }

    event.host_monotonic_ns = monotonic_ns();
    event.epoch_id = epoch_id;
    event.fpga_state = BLADERF_RF_STATE_RX_DATA_INVALID;
    event.event_type = BLADERF_RF_EVT_RX_FORMAT_UNSUPPORTED;
    event.flags = (uint32_t)format | transition_channel_flags;
    event.error_code = BLADERF_ERR_UNSUPPORTED;
    bladerf2_rf_event_append(board_data, &event);
}

void bladerf2_rx_layout_unsupported(struct bladerf *dev,
                                    bladerf_channel_layout layout,
                                    bool active_requirement_context)
{
    struct bladerf2_board_data *board_data;
    struct bladerf_rf_event event = {0};
    uint32_t transition_channel_flags;

    if (dev == NULL || dev->board_data == NULL) {
        return;
    }
    board_data = dev->board_data;
    MUTEX_LOCK(&board_data->rx_async_epoch_lock);
    transition_channel_flags =
        bladerf2_rx_transition_channel_event_flags(
            board_data->rf_transition_current_channel,
            board_data->rf_transition_epoch_contract_enabled);
    if (active_requirement_context &&
        board_data->rf_transition_rx_x2_host_data_required) {
        event.transaction_id =
            board_data->rf_transition_rx_x2_host_data_transaction_id;
        if (board_data->rf_transition_epoch_certified &&
            board_data->rf_transition_certified_epoch_event.transaction_id ==
                event.transaction_id) {
            event.epoch_id = board_data->rf_transition_certified_epoch_id;
        }
        bladerf2_rx_transition_fail_first_host_data_locked(
            board_data, BLADERF_ERR_UNSUPPORTED);
    }
    MUTEX_UNLOCK(&board_data->rx_async_epoch_lock);

    event.host_monotonic_ns = monotonic_ns();
    event.fpga_state = BLADERF_RF_STATE_RX_DATA_INVALID;
    event.event_type = BLADERF_RF_EVT_RX_LAYOUT_UNSUPPORTED;
    event.flags = (uint32_t)layout | transition_channel_flags;
    event.error_code = BLADERF_ERR_UNSUPPORTED;
    bladerf2_rf_event_append(board_data, &event);
}

static void _rx_data_withheld(struct bladerf *dev, uint32_t reason,
                              bool explicit_source,
                              bool explicit_timestamp_valid,
                              uint8_t event_epoch_id, uint64_t event_timestamp)
{
    struct bladerf2_board_data *board_data;
    struct bladerf_rf_event event = {0};
    uint8_t epoch_id;
    uint64_t first_unvalidated_timestamp = 0;
    bool timestamp_valid = false;
    bool should_report = false;
    uint32_t transition_channel_flags = 0;

    if (dev == NULL || dev->board_data == NULL) {
        return;
    }
    board_data = dev->board_data;

    /* This may run from the USB callback or sync parser. Keep it independent
     * of dev->lock, which can be held by a setter waiting for USB progress. */
    MUTEX_LOCK(&board_data->rx_async_epoch_lock);
    board_data->rx_async_data_withheld_active = true;
    if (reason == BLADERF_RF_WITHHELD_SHORT_TRANSFER ||
        reason == BLADERF_RF_WITHHELD_USB_OVERFLOW ||
        reason == BLADERF_RF_WITHHELD_USB_TRANSFER_ERROR ||
        reason == BLADERF_RF_WITHHELD_USB_TIMEOUT ||
        reason == BLADERF_RF_WITHHELD_DEVICE_LOST ||
        reason == BLADERF_RF_WITHHELD_SYNC_TIMEOUT) {
        /* A USB transport failure is invalid even in legacy mode, where the
         * epoch gate itself is disabled. Record each occurrence. */
        should_report = true;
    } else if (board_data->rf_transition_epoch_contract_enabled &&
               !board_data->rx_async_data_withheld_reported) {
        board_data->rx_async_data_withheld_reported = true;
        should_report = true;
    }
    epoch_id = explicit_source ? event_epoch_id :
                                board_data->rf_transition_epoch_id;
    /* A withheld async buffer is still associated with the transition that
     * established the shared RX epoch. Preserve the selected control channel
     * so wrappers can distinguish an RX1-initiated transition from RX2 while
     * still treating loss as affecting the shared two-lane epoch. */
    transition_channel_flags =
        bladerf2_rx_transition_channel_event_flags(
            board_data->rf_transition_current_channel,
            board_data->rf_transition_epoch_contract_enabled);
    /* This cursor is the first sample expected after the last admitted async
     * META buffer. Publish it only while it still belongs to the certified
     * epoch; never borrow it for a synchronous timeout or an invalid epoch. */
    if (!explicit_source && reason != BLADERF_RF_WITHHELD_SYNC_TIMEOUT &&
        board_data->rf_transition_epoch_certified &&
        board_data->rx_async_have_expected_timestamp &&
        board_data->rx_async_timestamp_epoch_id ==
            board_data->rf_transition_certified_epoch_id) {
        first_unvalidated_timestamp =
            board_data->rx_async_expected_timestamp;
        epoch_id = board_data->rf_transition_certified_epoch_id;
        timestamp_valid = true;
    }
    MUTEX_UNLOCK(&board_data->rx_async_epoch_lock);

    if (!should_report) {
        return;
    }

    event.host_monotonic_ns = monotonic_ns();
    event.fpga_timestamp = explicit_source
        ? event_timestamp : first_unvalidated_timestamp;
    event.epoch_id = epoch_id;
    event.fpga_state = BLADERF_RF_STATE_RX_DATA_INVALID;
    event.event_type = BLADERF_RF_EVT_RX_DATA_WITHHELD;
    event.flags = reason | ((explicit_source
                                 ? explicit_timestamp_valid
                                 : timestamp_valid)
        ? BLADERF_RF_EVENT_F_FPGA_TIMESTAMP_VALID : 0) |
        transition_channel_flags;
    bladerf2_rf_event_append(board_data, &event);
}

void bladerf2_rx_data_withheld(struct bladerf *dev, uint32_t reason)
{
    _rx_data_withheld(dev, reason, false, false, 0, 0);
}

uint32_t bladerf2_rx_current_transition_channel_event_flags(
    struct bladerf2_board_data *board_data)
{
    uint32_t flags;

    if (board_data == NULL) {
        return 0;
    }

    MUTEX_LOCK(&board_data->rx_async_epoch_lock);
    flags = bladerf2_rx_transition_channel_event_flags(
        board_data->rf_transition_current_channel,
        board_data->rf_transition_epoch_contract_enabled &&
            board_data->rf_transition_current_channel_valid);
    MUTEX_UNLOCK(&board_data->rx_async_epoch_lock);
    return flags;
}

uint32_t bladerf2_rx_invalidation_channel_event_flags(
    struct bladerf2_board_data *board_data, bladerf_channel channel,
    uint32_t reason)
{
    const uint32_t shared_reasons = BLADERF_RF_INVALIDATE_RFIC_REG |
                                    BLADERF_RF_INVALIDATE_TX_FIR |
                                    BLADERF_RF_INVALIDATE_CLOCK;

    if ((reason & shared_reasons) != 0) {
        /* These setters have no RX channel argument. Use the channel that
         * established the shared certified epoch; if there is no active
         * epoch contract, leave provenance unspecified. */
        return bladerf2_rx_current_transition_channel_event_flags(board_data);
    }

    return bladerf2_rx_transition_channel_event_flags(channel, true);
}

void bladerf2_rx_data_withheld_at(struct bladerf *dev, uint32_t reason,
                                  uint8_t epoch_id,
                                  uint64_t fpga_timestamp,
                                  bool fpga_timestamp_valid)
{
    _rx_data_withheld(dev, reason, true, fpga_timestamp_valid,
                      epoch_id, fpga_timestamp);
}

void bladerf2_rx_data_withheld_reset(struct bladerf *dev)
{
    struct bladerf2_board_data *board_data;

    if (dev == NULL || dev->board_data == NULL) {
        return;
    }
    board_data = dev->board_data;

    MUTEX_LOCK(&board_data->rx_async_epoch_lock);
    bladerf2_rx_data_rearm_notifications_locked(board_data);
    MUTEX_UNLOCK(&board_data->rx_async_epoch_lock);
}

void bladerf2_rx_data_rearm_notifications_locked(
    struct bladerf2_board_data *board_data)
{
    board_data->rx_format_unsupported_reported = false;
    board_data->rx_async_data_withheld_reported = false;
    board_data->rx_async_timestamp_discontinuity_reported = false;
}

void bladerf2_rx_data_note_first_packet_locked(
    struct bladerf2_board_data *board_data,
    const struct bladerf_metadata *metadata,
    bladerf_channel_layout layout)
{
    const struct bladerf_rf_event *epoch_event;
    struct bladerf_rf_event event = {0};
    bool already_reported;

    if (board_data == NULL || !metadata_rx_has_epoch_samples(metadata) ||
        (layout != BLADERF_RX_X1 && layout != BLADERF_RX_X2) ||
        !board_data->rf_transition_epoch_contract_enabled ||
        !board_data->rf_transition_epoch_certified ||
        (board_data->rf_transition_rx_x2_host_data_required &&
         layout != BLADERF_RX_X2)) {
        return;
    }

    epoch_event = &board_data->rf_transition_certified_epoch_event;
    if (epoch_event->event_type != BLADERF_RF_EVT_RX_EPOCH_VALID ||
        epoch_event->epoch_id != metadata->rx_epoch_id ||
        board_data->rf_transition_certified_epoch_id !=
            metadata->rx_epoch_id ||
        metadata->timestamp < board_data->rf_transition_first_valid_timestamp ||
        metadata->timestamp < epoch_event->fpga_timestamp) {
        return;
    }

    if (!bladerf2_rx_layout_matches_channel_mask(
            layout, board_data->rf_transition_current_channel,
            board_data->rx_channel_enable_mask_valid,
            board_data->rx_channel_enable_mask)) {
        bladerf2_rx_transition_fail_first_host_data_locked(
            board_data, BLADERF_ERR_UNSUPPORTED);
        if (!board_data->rx_async_data_withheld_active) {
            event.host_monotonic_ns = monotonic_ns();
            event.fpga_timestamp = metadata->timestamp;
            event.transaction_id = epoch_event->transaction_id;
            event.epoch_id = epoch_event->epoch_id;
            event.requested_rx_lo_hz = epoch_event->requested_rx_lo_hz;
            event.readback_rx_lo_hz = epoch_event->readback_rx_lo_hz;
            event.rfic_status = epoch_event->rfic_status;
            event.fpga_state = BLADERF_RF_STATE_RX_DATA_INVALID;
            event.event_type = BLADERF_RF_EVT_RX_DATA_WITHHELD;
            event.flags = BLADERF_RF_WITHHELD_RX_CHANNEL_SELECTION |
                          BLADERF_RF_EVENT_F_FPGA_TIMESTAMP_VALID |
                          (epoch_event->flags &
                           (BLADERF_RF_EVENT_F_TRANSITION_RX2 |
                            BLADERF_RF_EVENT_F_TRANSITION_CHANNEL_VALID));
            event.error_code = BLADERF_ERR_UNSUPPORTED;
            MUTEX_LOCK(&board_data->rf_transition_event_lock);
            bladerf2_rf_event_append_locked(board_data, &event);
            MUTEX_UNLOCK(&board_data->rf_transition_event_lock);
            board_data->rx_async_data_withheld_active = true;
        }
        return;
    }

    already_reported = board_data->rf_transition_first_host_data_reported;
    if (!already_reported &&
        board_data->rf_transition_first_host_data_required &&
        board_data->rf_transition_first_host_data_deadline_ns != 0 &&
        !bladerf2_rx_first_host_data_before_deadline(
            monotonic_ns(),
            board_data->rf_transition_first_host_data_deadline_ns)) {
        /* Sync admission calls this before copying to application memory;
         * async admission also reaches this point after its commit checks.
         * A late packet cannot be promoted to first-valid host data. */
        bladerf2_rx_transition_fail_first_host_data_locked(
            board_data, BLADERF_ERR_TIMEOUT);
        return;
    }
    if (!already_reported || board_data->rx_async_data_withheld_active) {
        event.host_monotonic_ns = monotonic_ns();
        event.fpga_timestamp = metadata->timestamp;
        event.transaction_id = epoch_event->transaction_id;
        event.epoch_id = epoch_event->epoch_id;
        event.requested_rx_lo_hz = epoch_event->requested_rx_lo_hz;
        event.readback_rx_lo_hz = epoch_event->readback_rx_lo_hz;
        event.rfic_status = epoch_event->rfic_status;
        event.fpga_state = BLADERF_RF_STATE_RX_DATA_VALID;
        event.event_type = already_reported
            ? BLADERF_RF_EVT_RX_DATA_RESUMED
            : BLADERF_RF_EVT_RX_FIRST_VALID_HOST_DATA;
        event.flags = metadata->status |
                      BLADERF_RF_EVENT_F_FPGA_TIMESTAMP_VALID;
        event.flags |= epoch_event->flags &
                       (BLADERF_RF_EVENT_F_TRANSITION_RX2 |
                        BLADERF_RF_EVENT_F_TRANSITION_CHANNEL_VALID);
        if (layout == BLADERF_RX_X2) {
            event.flags |= BLADERF_RF_EVENT_F_RX_X2_LAYOUT;
        }
        event.error_code = 0;

        MUTEX_LOCK(&board_data->rf_transition_event_lock);
        bladerf2_rf_event_append_locked(board_data, &event);
        MUTEX_UNLOCK(&board_data->rf_transition_event_lock);
        if (!already_reported) {
            board_data->rf_transition_first_host_data_event = event;
        }
        board_data->rf_transition_first_host_data_reported = true;
        board_data->rx_async_data_withheld_active = false;
        if (!already_reported) {
            COND_SIGNAL(&board_data->rx_async_epoch_cond);
        }
    }
    bladerf2_rx_data_rearm_notifications_locked(board_data);
}

uint32_t bladerf2_rx_sync_data_admission_reason_locked(
    struct bladerf2_board_data *board_data,
    const struct bladerf_metadata *metadata,
    bladerf_channel_layout layout)
{
    if (board_data == NULL || metadata == NULL ||
        (layout != BLADERF_RX_X1 && layout != BLADERF_RX_X2)) {
        return BLADERF_RF_WITHHELD_EPOCH_UNCERTIFIED;
    }
    if (!board_data->rf_transition_epoch_contract_enabled) {
        return 0;
    }
    if (!board_data->rf_transition_epoch_certified ||
        !metadata->rx_epoch_id_valid ||
        metadata->rx_epoch_id != board_data->rf_transition_certified_epoch_id ||
        metadata->timestamp < board_data->rf_transition_first_valid_timestamp) {
        return BLADERF_RF_WITHHELD_EPOCH_UNCERTIFIED;
    }
    if (!bladerf2_rx_layout_matches_channel_mask(
            layout, board_data->rf_transition_current_channel,
            board_data->rx_channel_enable_mask_valid,
            board_data->rx_channel_enable_mask)) {
        bladerf2_rx_transition_fail_first_host_data_locked(
            board_data, BLADERF_ERR_UNSUPPORTED);
        return BLADERF_RF_WITHHELD_RX_CHANNEL_SELECTION;
    }
    if (board_data->rf_transition_first_host_data_required &&
        !board_data->rf_transition_first_host_data_reported) {
        const uint64_t deadline_ns =
            board_data->rf_transition_first_host_data_deadline_ns;
        struct timespec now_ts;
        uint64_t now_ns = 0;
        if (clock_gettime(CLOCK_MONOTONIC, &now_ts) == 0) {
            now_ns = (uint64_t)now_ts.tv_sec * 1000000000ULL +
                     (uint64_t)now_ts.tv_nsec;
        }
        /* A zero deadline means wait() has not armed its watchdog yet. The
         * wait path validates the event timestamp once its deadline exists. */
        if (deadline_ns != 0 && (now_ns == 0 || now_ns >= deadline_ns)) {
            bladerf2_rx_transition_fail_first_host_data_locked(
                board_data, BLADERF_ERR_TIMEOUT);
            return BLADERF_RF_WITHHELD_SYNC_TIMEOUT;
        }
        bladerf2_rx_data_note_first_packet_locked(board_data, metadata,
                                                   layout);
        if (!board_data->rf_transition_first_host_data_reported) {
            return board_data->rf_transition_first_host_data_failure ==
                   BLADERF_ERR_TIMEOUT
                ? BLADERF_RF_WITHHELD_SYNC_TIMEOUT
                : BLADERF_RF_WITHHELD_EPOCH_UNCERTIFIED;
        }
    }
    return 0;
}

void bladerf2_rx_async_timestamp_discontinuity(
    struct bladerf *dev, uint8_t expected_epoch_id,
    uint64_t first_unvalidated_timestamp)
{
    struct bladerf2_board_data *board_data;
    struct bladerf_rf_event event = {0};
    uint8_t epoch_id;
    bool timestamp_valid;
    bool should_report = false;
    uint32_t transition_channel_flags = 0;

    if (dev == NULL || dev->board_data == NULL) {
        return;
    }
    board_data = dev->board_data;

    /* This runs in the asynchronous USB callback. Keep it independent of
     * dev->lock, which may be held by a setter waiting for USB progress. */
    MUTEX_LOCK(&board_data->rx_async_epoch_lock);
    if (board_data->rf_transition_epoch_contract_enabled &&
        !board_data->rx_async_timestamp_discontinuity_reported) {
        board_data->rx_async_timestamp_discontinuity_reported = true;
        board_data->rx_async_data_withheld_active = true;
        should_report = true;
    }
    timestamp_valid = board_data->rf_transition_epoch_contract_enabled &&
        board_data->rf_transition_epoch_certified &&
        board_data->rf_transition_certified_epoch_id == expected_epoch_id;
    transition_channel_flags =
        bladerf2_rx_transition_channel_event_flags(
            board_data->rf_transition_current_channel,
            board_data->rf_transition_epoch_contract_enabled);
    epoch_id = timestamp_valid
        ? expected_epoch_id : board_data->rf_transition_epoch_id;
    MUTEX_UNLOCK(&board_data->rx_async_epoch_lock);

    if (!should_report) {
        return;
    }

    event.host_monotonic_ns = monotonic_ns();
    event.fpga_timestamp = timestamp_valid
        ? first_unvalidated_timestamp : 0;
    event.epoch_id = epoch_id;
    event.fpga_state = BLADERF_RF_STATE_RX_DATA_INVALID;
    event.event_type = BLADERF_RF_EVT_RX_DATA_WITHHELD;
    event.flags = BLADERF_RF_WITHHELD_TIMESTAMP_DISCONTINUITY |
        (timestamp_valid ? BLADERF_RF_EVENT_F_FPGA_TIMESTAMP_VALID : 0) |
        transition_channel_flags;
    event.error_code = BLADERF_ERR_UNEXPECTED;
    bladerf2_rf_event_append(board_data, &event);

    event.host_monotonic_ns = monotonic_ns();
    event.event_type = BLADERF_RF_EVT_RX_STREAM_OVERRUN;
    event.flags = BLADERF_RF_STREAM_STATUS_OVERRUN |
                  BLADERF_RF_STREAM_STATUS_TIMESTAMP_DISCONTINUITY |
                  transition_channel_flags;
    event.error_code = 0;
    bladerf2_rf_event_append(board_data, &event);
}

void bladerf2_rx_async_stream_overrun(struct bladerf *dev)
{
    struct bladerf2_board_data *board_data;
    struct bladerf_rf_event event = {0};

    if (dev == NULL || dev->board_data == NULL) {
        return;
    }
    board_data = dev->board_data;

    /* The libusb callback cannot wait for dev->lock: a setter may own it while
     * blocked on this same USB event loop. Only snapshot lock-safe identity. */
    MUTEX_LOCK(&board_data->rx_async_epoch_lock);
    event.epoch_id = board_data->rf_transition_certified_epoch_id;
    event.flags = BLADERF_RF_STREAM_STATUS_OVERRUN |
        BLADERF_RF_STREAM_STATUS_ASYNC_USB |
        bladerf2_rx_transition_channel_event_flags(
            board_data->rf_transition_current_channel,
            board_data->rf_transition_epoch_contract_enabled);
    board_data->rx_async_data_withheld_active = true;
    MUTEX_UNLOCK(&board_data->rx_async_epoch_lock);

    event.host_monotonic_ns = monotonic_ns();
    event.fpga_state = BLADERF_RF_STATE_RX_DATA_INVALID;
    event.event_type = BLADERF_RF_EVT_RX_STREAM_OVERRUN;
    bladerf2_rf_event_append(board_data, &event);
}

void bladerf2_rx_worker_stream_overrun(struct bladerf *dev,
                                      uint32_t source_flags)
{
    struct bladerf2_board_data *board_data;
    struct bladerf_rf_event event = {0};

    if (dev == NULL || dev->board_data == NULL || source_flags == 0) {
        return;
    }
    board_data = dev->board_data;

    /* Called from the sync RX worker while its ring lock may be held. Never
     * acquire dev->lock here: configuration setters can own it while waiting
     * for USB progress. Snapshot only the lock-protected epoch identity. */
    MUTEX_LOCK(&board_data->rx_async_epoch_lock);
    event.epoch_id = board_data->rf_transition_epoch_certified
        ? board_data->rf_transition_certified_epoch_id
        : board_data->rf_transition_epoch_id;
    event.fpga_state = board_data->rf_transition_epoch_certified
        ? BLADERF_RF_STATE_RX_DATA_VALID
        : BLADERF_RF_STATE_RX_DATA_INVALID;
    source_flags |= bladerf2_rx_transition_channel_event_flags(
        board_data->rf_transition_current_channel,
        board_data->rf_transition_epoch_contract_enabled);
    if (board_data->rf_transition_epoch_contract_enabled) {
        board_data->rx_async_data_withheld_active = true;
    }
    MUTEX_UNLOCK(&board_data->rx_async_epoch_lock);

    event.host_monotonic_ns = monotonic_ns();
    event.event_type = BLADERF_RF_EVT_RX_STREAM_OVERRUN;
    event.flags = BLADERF_RF_STREAM_STATUS_OVERRUN | source_flags;
    event.error_code = 0;
    bladerf2_rf_event_append(board_data, &event);
}
