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

void bladerf2_rx_async_data_withheld(struct bladerf *dev, uint32_t reason)
{
    struct bladerf2_board_data *board_data;
    struct bladerf_rf_event event = {0};
    uint8_t epoch_id;
    bool should_report = false;

    if (dev == NULL || dev->board_data == NULL) {
        return;
    }
    board_data = dev->board_data;

    /* This is called from the USB callback; keep it independent of dev->lock. */
    MUTEX_LOCK(&board_data->rx_async_epoch_lock);
    if (board_data->rf_transition_epoch_contract_enabled &&
        !board_data->rx_async_data_withheld_reported) {
        board_data->rx_async_data_withheld_reported = true;
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
    event.event_type = BLADERF_RF_EVT_RX_DATA_WITHHELD;
    event.flags = reason;
    bladerf2_rf_event_append(board_data, &event);
}

void bladerf2_rx_async_timestamp_discontinuity(struct bladerf *dev)
{
    struct bladerf2_board_data *board_data;
    struct bladerf_rf_event event = {0};
    uint8_t epoch_id;
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
        should_report = true;
    }
    epoch_id = board_data->rf_transition_certified_epoch_id;
    MUTEX_UNLOCK(&board_data->rx_async_epoch_lock);

    if (!should_report) {
        return;
    }

    event.host_monotonic_ns = monotonic_ns();
    event.epoch_id = epoch_id;
    event.fpga_state = BLADERF_RF_STATE_RX_DATA_INVALID;
    event.event_type = BLADERF_RF_EVT_RX_DATA_WITHHELD;
    event.flags = BLADERF_RF_WITHHELD_TIMESTAMP_DISCONTINUITY;
    event.error_code = BLADERF_ERR_UNEXPECTED;
    bladerf2_rf_event_append(board_data, &event);

    event.host_monotonic_ns = monotonic_ns();
    event.event_type = BLADERF_RF_EVT_RX_STREAM_OVERRUN;
    event.flags = BLADERF_RF_STREAM_STATUS_OVERRUN;
    event.error_code = 0;
    bladerf2_rf_event_append(board_data, &event);
}
