/* Copyright 2026 Nuand LLC
 *
 * This file is part of the bladeRF project and is distributed under the
 * terms of the GNU Lesser General Public License, version 2.1 or later.
 */
#ifndef BLADERF2_TX_FAULT_RECOVERY_H_
#define BLADERF2_TX_FAULT_RECOVERY_H_

#include <stdint.h>

/* Board-owned stale-TX recovery after the fault-flush latch is set. The
 * sequence is fixed; each step is an operation so the order and the
 * fail-closed exits can be exercised without hardware. */
struct bladerf2_tx_fault_recovery_ops {
    /* Called without the board lock: retire accepted TX submissions and
     * their completion callbacks. Partial transfers are never canceled. */
    int (*drain_submissions)(void *ctx, unsigned int timeout_ms);
    void (*lock)(void *ctx);
    void (*unlock)(void *ctx);
    /* Locked. Require the latch and zero submit leases; snapshot the fault
     * generation that this pass is allowed to retire. */
    int (*begin)(void *ctx, uint64_t *fault_generation);
    /* Block FPGA TX FIFO writes and abort the faulted GPIF TX parse. */
    int (*fifo_flush_start)(void *ctx);
    /* Writer gate observed and GPIF parser idle (not FIFO empty). */
    int (*fifo_flush_wait)(void *ctx, unsigned int timeout_ms);
    /* CLEAR_FEATURE(ENDPOINT_HALT) on the TX sample endpoint: FX3 resets
     * and rearms the DMA channel, discarding stale complete/partial data. */
    int (*clear_tx_endpoint_halt)(void *ctx);
    /* The AD9361 data clock must be present for the FIFO clear to release.
     * Returns BLADERF_ERR_WOULD_BLOCK while the RFIC needs reinitialization. */
    int (*rfic_ready)(void *ctx);
    /* Fresh TX clock, both FIFO reset-release pipelines and empty flags. */
    int (*fifo_flush_release)(void *ctx, unsigned int timeout_ms);
    /* Clear the latch only if no newer fault was latched since begin(). */
    int (*commit)(void *ctx, uint64_t fault_generation);
};

/* Every hardware step is idempotent, so a failed pass leaves the latch set
 * and may simply be repeated. timeout_ms bounds each waiting step. */
int bladerf2_tx_fault_recovery_run(
    const struct bladerf2_tx_fault_recovery_ops *ops, void *ctx,
    unsigned int timeout_ms);

#endif
