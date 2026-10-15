/**
 * Copyright 2026 bladeRF project contributors
 *
 * This file is part of the bladeRF project.
 *
 * This is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation, either version 2.1
 * of the License, or (at your option) any later version.
 */
#include <libbladeRF.h>

#include "board/board.h"
#include "board/bladerf2/common.h"
#include "helpers/timeout.h"
#include "streaming/sync.h"

struct rx_fault_event_publish_context {
    struct bladerf2_board_data *board_data;
    struct bladerf_rf_event event;
    uint32_t channel_flags;
};

/* Run while the sync generation lock is held. */
static void _revoke_rx_fault_and_publish_event(void *context)
{
    struct rx_fault_event_publish_context *publish = context;

    bladerf2_rx_epoch_revoke_admission(publish->board_data);
    bladerf2_rf_event_append_rx_invalidation(
        publish->board_data, &publish->event, publish->channel_flags);
}

/* A poisoned AD9361 invalidates the shared RX signal chain regardless of
 * which channel or API caused the failure. Do not take dev->lock here: RFIC
 * callbacks can arrive while the public operation already owns that mutex. */
void bladerf2_rx_rfic_state_invalid(struct bladerf *dev, int error_code)
{
    struct bladerf2_board_data *board_data;
    struct rx_fault_event_publish_context publish = {0};
    bool report_event;

    if (dev == NULL || dev->board_data == NULL) {
        return;
    }
    board_data = dev->board_data;

    MUTEX_LOCK(&board_data->rx_async_epoch_lock);
    report_event = !board_data->rx_rfic_fault_event_reported;
    board_data->rx_rfic_fault_event_reported = true;
    if (report_event) {
        (void)bladerf_monotonic_now_ns(&publish.event.host_monotonic_ns);
        publish.event.event_type = BLADERF_RF_EVT_RX_DATA_INVALIDATED;
        publish.event.fpga_state = BLADERF_RF_STATE_RX_DATA_INVALID;
        publish.event.flags = BLADERF_RF_INVALIDATE_RFIC_REG;
        publish.event.error_code = error_code;
        publish.board_data = board_data;
        /* No transition/epoch attribution is claimed: this may be a legacy
         * ADI/IIO operation or a shared-clock mutation outside an RX epoch. */
        publish.channel_flags = 0;
    }
    MUTEX_UNLOCK(&board_data->rx_async_epoch_lock);

    if (report_event) {
        sync_rx_epoch_revoke_delivery_with_callback(
            &board_data->sync[BLADERF_RX],
            _revoke_rx_fault_and_publish_event, &publish);
    } else {
        /* Coalesce notifications per poison episode but keep admission shut. */
        bladerf2_rx_epoch_revoke_admission(board_data);
    }
}
