/*
 * This file is part of the bladeRF project:
 *   http://www.github.com/nuand/bladeRF
 *
 * Copyright (C) 2026 Nuand LLC
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "board/board.h"
#include "board/bladerf2/common.h"

/* Called by the board pre-close hook before bladerf_close() acquires
 * dev->lock. Wake transition calls and let them complete fail-closed cleanup
 * before their condition variable, mutex, or board_data are destroyed. */
void bladerf2_rx_transition_waiters_close(struct bladerf *dev)
{
    struct bladerf2_board_data *board_data;

    if (dev == NULL || dev->board_data == NULL) {
        return;
    }
    board_data = dev->board_data;
    if (!board_data->rx_async_epoch_lock_initialized ||
        !board_data->rx_async_epoch_cond_initialized) {
        return;
    }

    MUTEX_LOCK(&board_data->rx_async_epoch_lock);
    board_data->rf_transition_close_requested = true;
    COND_SIGNAL(&board_data->rx_async_epoch_cond);
    while (board_data->rf_transition_call_count != 0) {
        COND_WAIT(&board_data->rx_async_epoch_cond,
                  &board_data->rx_async_epoch_lock);
    }
    MUTEX_UNLOCK(&board_data->rx_async_epoch_lock);
}

int bladerf2_rx_transition_call_enter(
    struct bladerf *dev, struct bladerf2_board_data **board_data_out)
{
    struct bladerf2_board_data *board_data;
    int status = 0;

    if (dev == NULL || board_data_out == NULL) {
        return BLADERF_ERR_INVAL;
    }
    *board_data_out = NULL;
    /* Reject unsupported boards before touching a possibly uninitialized
     * device mutex, matching the transition API's existing contract. */
    if (dev->board != &bladerf2_board_fns) {
        return BLADERF_ERR_UNSUPPORTED;
    }

    /* Count the operation before releasing dev->lock. Pre-close waits for
     * this count before generic close can acquire the lock and free state. */
    MUTEX_LOCK(&dev->lock);
    if (dev->board_data == NULL) {
        status = BLADERF_ERR_INVAL;
    } else {
        board_data = dev->board_data;
        if (!board_data->rx_async_epoch_lock_initialized ||
            !board_data->rx_async_epoch_cond_initialized) {
            status = BLADERF_ERR_NOT_INIT;
        } else {
            MUTEX_LOCK(&board_data->rx_async_epoch_lock);
            if (board_data->rf_transition_close_requested) {
                status = BLADERF_ERR_ABORTED;
            } else {
                board_data->rf_transition_call_count++;
                *board_data_out = board_data;
            }
            MUTEX_UNLOCK(&board_data->rx_async_epoch_lock);
        }
    }
    MUTEX_UNLOCK(&dev->lock);
    return status;
}

void bladerf2_rx_transition_call_leave(
    struct bladerf2_board_data *board_data)
{
    if (board_data == NULL) {
        return;
    }
    MUTEX_LOCK(&board_data->rx_async_epoch_lock);
    if (board_data->rf_transition_call_count != 0) {
        board_data->rf_transition_call_count--;
    }
    COND_SIGNAL(&board_data->rx_async_epoch_cond);
    MUTEX_UNLOCK(&board_data->rx_async_epoch_lock);
}
