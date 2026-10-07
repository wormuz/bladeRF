/* Copyright 2026 Nuand LLC
 *
 * This file is part of the bladeRF project and is distributed under the
 * terms of the GNU Lesser General Public License, version 2.1 or later.
 */
#include <time.h>

#include <libbladeRF.h>

#include "bladeRF.h"
#include "board/board.h"
#include "common.h"

static uint64_t monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
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

void bladerf2_rx_async_format_unsupported(struct bladerf *dev,
                                          bladerf_format format)
{
    struct bladerf2_board_data *board_data;
    struct bladerf_rf_event event = {0};
    uint8_t epoch_id;
    bool should_report = false;

    if (dev == NULL || dev->board_data == NULL) {
        return;
    }
    board_data = dev->board_data;

    /* Runs in the async USB callback. Do not take dev->lock: a setter may
     * hold it while waiting for progress from this USB event loop. */
    MUTEX_LOCK(&board_data->rx_async_epoch_lock);
    if (board_data->rf_transition_epoch_contract_enabled &&
        !board_data->rx_async_format_unsupported_reported) {
        board_data->rx_async_format_unsupported_reported = true;
        board_data->rx_async_data_withheld_active = true;
        should_report = true;
    }
    epoch_id = board_data->rf_transition_epoch_id;
    MUTEX_UNLOCK(&board_data->rx_async_epoch_lock);

    if (!should_report) {
        return;
    }

    event.host_monotonic_ns = monotonic_ns();
    event.epoch_id = epoch_id;
    event.fpga_state = BLADERF_RF_STATE_RX_DATA_INVALID;
    event.event_type = BLADERF_RF_EVT_RX_FORMAT_UNSUPPORTED;
    event.flags = (uint32_t)format;
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
        ? BLADERF_RF_EVENT_F_FPGA_TIMESTAMP_VALID : 0);
    bladerf2_rf_event_append(board_data, &event);
}

void bladerf2_rx_data_withheld(struct bladerf *dev, uint32_t reason)
{
    _rx_data_withheld(dev, reason, false, false, 0, 0);
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
    board_data->rx_async_data_withheld_reported = false;
    board_data->rx_async_timestamp_discontinuity_reported = false;
    MUTEX_UNLOCK(&board_data->rx_async_epoch_lock);
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
        (timestamp_valid ? BLADERF_RF_EVENT_F_FPGA_TIMESTAMP_VALID : 0);
    event.error_code = BLADERF_ERR_UNEXPECTED;
    bladerf2_rf_event_append(board_data, &event);

    event.host_monotonic_ns = monotonic_ns();
    event.event_type = BLADERF_RF_EVT_RX_STREAM_OVERRUN;
    event.flags = BLADERF_RF_STREAM_STATUS_OVERRUN;
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
    board_data->rx_async_data_withheld_active = true;
    MUTEX_UNLOCK(&board_data->rx_async_epoch_lock);

    event.host_monotonic_ns = monotonic_ns();
    event.fpga_state = BLADERF_RF_STATE_RX_DATA_INVALID;
    event.event_type = BLADERF_RF_EVT_RX_STREAM_OVERRUN;
    event.flags = BLADERF_RF_STREAM_STATUS_OVERRUN;
    bladerf2_rf_event_append(board_data, &event);
}
