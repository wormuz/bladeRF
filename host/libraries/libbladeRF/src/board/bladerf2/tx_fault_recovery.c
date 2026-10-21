/* Copyright 2026 Nuand LLC
 *
 * This file is part of the bladeRF project and is distributed under the
 * terms of the GNU Lesser General Public License, version 2.1 or later.
 */
#include <stddef.h>

#include <libbladeRF.h>

#include "tx_fault_recovery.h"

int bladerf2_tx_fault_recovery_run(
    const struct bladerf2_tx_fault_recovery_ops *ops, void *ctx,
    unsigned int timeout_ms)
{
    uint64_t fault_generation = 0;
    int status;

    if (ops == NULL) {
        return BLADERF_ERR_INVAL;
    }

    /* Leases retire from USB callbacks; waiting here with the board lock
     * held would stall the callback pump that retires them. */
    status = ops->drain_submissions(ctx, timeout_ms);
    if (status != 0) {
        return status;
    }

    ops->lock(ctx);
    status = ops->begin(ctx, &fault_generation);
    if (status == 0) {
        status = ops->fifo_flush_start(ctx);
    }
    if (status == 0) {
        status = ops->fifo_flush_wait(ctx, timeout_ms);
    }
    /* FX3 may still own a partial buffer; reset the endpoint only after
     * the FPGA has stopped consuming and the GPIF parser is idle. */
    if (status == 0) {
        status = ops->clear_tx_endpoint_halt(ctx);
    }
    if (status == 0) {
        status = ops->rfic_ready(ctx);
    }
    if (status == 0) {
        status = ops->fifo_flush_release(ctx, timeout_ms);
    }
    if (status == 0) {
        status = ops->commit(ctx, fault_generation);
    }
    ops->unlock(ctx);
    return status;
}
