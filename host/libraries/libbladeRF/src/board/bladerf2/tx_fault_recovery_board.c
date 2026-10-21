/* Copyright 2026 Nuand LLC
 *
 * This file is part of the bladeRF project and is distributed under the
 * terms of the GNU Lesser General Public License, version 2.1 or later.
 */
#include <libbladeRF.h>

#include "bladeRF.h"
#include "host_config.h"
#include "board/board.h"
#include "bladerf2_common.h"
#include "common.h"
#include "helpers/timeout.h"
#include "log.h"
#include "backend/usb/nios_access.h"
#include "backend/usb/usb.h"

#include "tx_fault_recovery.h"

struct tx_fault_recovery_ctx {
    struct bladerf *dev;
    struct bladerf2_board_data *board_data;
};

static int _drain_submissions(void *arg, unsigned int timeout_ms)
{
    struct tx_fault_recovery_ctx *ctx = arg;

    if (ctx->dev->board->wait_tx_async_idle == NULL) {
        return BLADERF_ERR_UNSUPPORTED;
    }
    /* Returns BLADERF_ERR_INVAL when no fault-flush latch is set. */
    return ctx->dev->board->wait_tx_async_idle(ctx->dev, timeout_ms);
}

static void _lock(void *arg)
{
    struct tx_fault_recovery_ctx *ctx = arg;
    MUTEX_LOCK(&ctx->dev->lock);
}

static void _unlock(void *arg)
{
    struct tx_fault_recovery_ctx *ctx = arg;
    MUTEX_UNLOCK(&ctx->dev->lock);
}

static int _begin(void *arg, uint64_t *fault_generation)
{
    struct tx_fault_recovery_ctx *ctx = arg;
    struct bladerf2_board_data *board_data = ctx->board_data;
    int status = 0;

    if (board_data->state < STATE_INITIALIZED) {
        return BLADERF_ERR_NOT_INIT;
    }

    MUTEX_LOCK(&board_data->rx_async_epoch_lock);
    if (!board_data->tx_async_fault_flush_required) {
        status = BLADERF_ERR_INVAL;
    } else if (board_data->tx_async_submit_leases != 0) {
        /* The drain ran without dev->lock; the latch closes admission, so a
         * nonzero count here is a lease that has not retired yet. */
        status = BLADERF_ERR_WOULD_BLOCK;
    } else {
        *fault_generation = board_data->tx_async_fault_flush_generation;
    }
    MUTEX_UNLOCK(&board_data->rx_async_epoch_lock);
    return status;
}

static int _fifo_flush_start(void *arg)
{
    struct tx_fault_recovery_ctx *ctx = arg;
    return nios_tx_fifo_fault_flush_start(ctx->dev);
}

static int _fifo_flush_wait(void *arg, unsigned int timeout_ms)
{
    struct tx_fault_recovery_ctx *ctx = arg;
    return nios_tx_fifo_fault_flush_wait(ctx->dev, timeout_ms);
}

static int _clear_tx_endpoint_halt(void *arg)
{
    struct tx_fault_recovery_ctx *ctx = arg;
    return usb_clear_endpoint_halt(ctx->dev, SAMPLE_EP_OUT);
}

static int _rfic_ready(void *arg)
{
    struct tx_fault_recovery_ctx *ctx = arg;
    struct bladerf2_board_data *board_data = ctx->board_data;
    bladerf_rfic_init_state init_state;
    int status;

    if (board_data->rfic == NULL ||
        board_data->rfic->get_init_state == NULL) {
        return BLADERF_ERR_UNEXPECTED;
    }
    /* This coordinator does not reset the shared AD9361: RESETB and full
     * init belong to the RFIC initialization path, which also accounts for
     * RX. A poisoned or stopped RFIC has no trustworthy data clock, so the
     * FIFO clear cannot complete; keep the latch and let the caller retry
     * after reinitialization. */
    if (board_data->calibration_faulted ||
        (board_data->phy != NULL &&
         ad9361_calibration_state_unknown(board_data->phy))) {
        return BLADERF_ERR_WOULD_BLOCK;
    }
    status = board_data->rfic->get_init_state(ctx->dev, &init_state);
    if (status != 0) {
        return status;
    }
    return init_state == BLADERF_RFIC_INIT_STATE_ON ? 0
                                                    : BLADERF_ERR_WOULD_BLOCK;
}

static int _fifo_flush_release(void *arg, unsigned int timeout_ms)
{
    struct tx_fault_recovery_ctx *ctx = arg;
    return nios_tx_fifo_fault_flush_release(ctx->dev, timeout_ms);
}

static int _commit(void *arg, uint64_t fault_generation)
{
    struct tx_fault_recovery_ctx *ctx = arg;
    struct bladerf2_board_data *board_data = ctx->board_data;
    struct bladerf_rf_event event = {0};
    uint64_t now_ns = 0;
    int status = 0;

    (void)bladerf_monotonic_now_ns(&now_ns);
    MUTEX_LOCK(&board_data->rx_async_epoch_lock);
    if (board_data->tx_async_fault_flush_generation != fault_generation) {
        /* A fault latched after begin() may postdate the FIFO flush. */
        status = BLADERF_ERR_WOULD_BLOCK;
    } else {
        board_data->tx_async_fault_flush_required = false;
        /* The flush certifies the transport path is empty, not that TX is
         * valid: a fresh link START and RFIC/gate certification are still
         * required before admission reopens. */
        board_data->tx_stream_faulted_async = true;
        board_data->tx_async_link_certified = false;
        board_data->tx_async_rfic_state_certified = false;
        board_data->tx_async_admission_generation++;
        event.host_monotonic_ns = now_ns;
        event.event_type = BLADERF_RF_EVT_TX_FAULT_FLUSH_COMPLETE;
        event.epoch_id = board_data->tx_async_link_epoch_id;
        bladerf2_rf_event_append(board_data, &event);
        COND_BROADCAST(&board_data->rx_async_epoch_cond);
    }
    MUTEX_UNLOCK(&board_data->rx_async_epoch_lock);
    return status;
}

static const struct bladerf2_tx_fault_recovery_ops tx_fault_recovery_ops = {
    FIELD_INIT(.drain_submissions, _drain_submissions),
    FIELD_INIT(.lock, _lock),
    FIELD_INIT(.unlock, _unlock),
    FIELD_INIT(.begin, _begin),
    FIELD_INIT(.fifo_flush_start, _fifo_flush_start),
    FIELD_INIT(.fifo_flush_wait, _fifo_flush_wait),
    FIELD_INIT(.clear_tx_endpoint_halt, _clear_tx_endpoint_halt),
    FIELD_INIT(.rfic_ready, _rfic_ready),
    FIELD_INIT(.fifo_flush_release, _fifo_flush_release),
    FIELD_INIT(.commit, _commit),
};

int bladerf_tx_fault_recover(struct bladerf *dev, unsigned int timeout_ms)
{
    struct tx_fault_recovery_ctx ctx;
    int status;

    if (dev == NULL || dev->board != &bladerf2_board_fns ||
        dev->board_data == NULL || timeout_ms == 0) {
        return BLADERF_ERR_INVAL;
    }

    ctx.dev = dev;
    ctx.board_data = dev->board_data;
    status = bladerf2_tx_fault_recovery_run(&tx_fault_recovery_ops, &ctx,
                                            timeout_ms);
    if (status != 0) {
        log_warning("TX fault recovery incomplete: %s; TX stays fenced\n",
                    bladerf_strerror(status));
    }
    return status;
}
