/* Copyright 2026 Nuand LLC
 *
 * This file is part of the bladeRF project and is distributed under the
 * terms of the GNU Lesser General Public License, version 2.1 or later.
 */
#include <libbladeRF.h>

#include "bladeRF.h"
#include "board/board.h"
#include "bladerf2_common.h"
#include "common.h"
#include "helpers/timeout.h"

bool bladerf2_tx_status_recovery_event(struct bladerf *dev,
                                       uint32_t link_status)
{
    struct bladerf2_board_data *board_data;
    struct bladerf_rf_event event = {0};
    uint64_t now_ns = 0;

    if (dev == NULL || dev->board_data == NULL) {
        return false;
    }

    board_data = dev->board_data;
    MUTEX_LOCK(&board_data->rx_async_epoch_lock);
    if (board_data->tx_async_transport_faulted ||
        board_data->tx_gate_abort_pending) {
        MUTEX_UNLOCK(&board_data->rx_async_epoch_lock);
        return false;
    }

    board_data->tx_stream_faulted_async = false;
    board_data->tx_async_link_certified = true;
    board_data->tx_async_rfic_state_certified = true;
    board_data->tx_async_admission_generation++;
    /* Start a new deduplication window only after the monitor has observed
     * a healthy RFIC and acknowledged TX link epoch. Configuration changes
     * are not recovery evidence on their own. */
    board_data->tx_rf_configuration_invalid_reported = false;
    (void)bladerf_monotonic_now_ns(&now_ns);
    event.host_monotonic_ns = now_ns;
    event.event_type = BLADERF_RF_EVT_TX_STATUS_RECOVERED;
    event.epoch_id = (uint8_t)(link_status >> 20);
    event.fpga_state = BLADERF_RF_STATE_IDLE;
    bladerf2_rf_event_append(board_data, &event);
    COND_SIGNAL(&board_data->rx_async_epoch_cond);
    MUTEX_UNLOCK(&board_data->rx_async_epoch_lock);
    return true;
}
