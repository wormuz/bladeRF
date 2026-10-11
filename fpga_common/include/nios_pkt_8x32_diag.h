/*
 * Copyright (c) 2026 Nuand LLC
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#ifndef NIOS_PKT_8X32_DIAG_H_
#define NIOS_PKT_8X32_DIAG_H_

#include <stdbool.h>
#include <stdint.h>

#include "nios_pkt_8x32.h"

/* Keep xA4 diagnostic subaddress bounds shared with the host-side packet
 * regression. Unknown targets are not handled here. */
static inline bool nios_pkt_8x32_diagnostic_read_addr_valid(uint8_t target,
                                                             uint8_t addr)
{
    switch (target) {
        case NIOS_PKT_8x32_TARGET_VERSION:
        case NIOS_PKT_8x32_TARGET_CONTROL:
        case NIOS_PKT_8x32_TARGET_RFFE_CSR:
            return addr == 0;

        case NIOS_PKT_8x32_TARGET_ADF400X:
            /* Register index is encoded by the packet address. The device
             * cache has four entries; rejecting larger values avoids an
             * out-of-contract read aliasing onto a valid register. */
            return addr <= 3;

        case NIOS_PKT_8x32_TARGET_RETUNE2_FAILURES:
        case NIOS_PKT_8x32_TARGET_RETUNE2_SUCCESSES:
            return addr <= NIOS_PKT_8x32_RETUNE2_FAILURES_TX_ADDR;

        case NIOS_PKT_8x32_TARGET_RETUNE2_COMPLETION:
            return addr <= NIOS_PKT_8x32_RETUNE2_COMPLETION_TS_HI;

        case NIOS_PKT_8x32_TARGET_LOSS_COUNTERS:
            return addr <= 3;

        case NIOS_PKT_8x32_TARGET_DWELL_READOUT:
            return addr <= 15;

        case NIOS_PKT_8x32_TARGET_RF_LINK_STATUS:
        case NIOS_PKT_8x32_TARGET_RX_FAULT_CAUSES:
        case NIOS_PKT_8x32_TARGET_DWELL_STATUS:
        case NIOS_PKT_8x32_TARGET_RX_EPOCH_STATUS:
        case NIOS_PKT_8x32_TARGET_RX_EPOCH_TS_LO:
        case NIOS_PKT_8x32_TARGET_RX_EPOCH_TS_HI:
            return addr == 0;

        case NIOS_PKT_8x32_TARGET_TX_RETUNE_GATE:
            return addr == NIOS_PKT_8x32_TX_RETUNE_CMD_STATUS;

        default:
            return true;
    }
}

#endif
