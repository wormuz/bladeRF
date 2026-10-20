/* This file is part of the bladeRF project:
 *   http://www.github.com/nuand/bladeRF
 *
 * Copyright (C) 2026 Nuand LLC
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301
 * USA
 */
#ifndef BLADERF2_RX_SWEEP_POLICY_H_
#define BLADERF2_RX_SWEEP_POLICY_H_

#include <stdbool.h>
#include <stdint.h>

#include <libbladeRF.h>

#include "nios_pkt_retune2.h"

/* AD9361 gain-table region, as ad9361_gt_tableindex() selects it:
 * start < f <= end over ad9361_adi_gt_info. */
static inline unsigned int bladerf2_rx_gain_table_region(uint64_t frequency)
{
    if (frequency <= BLADERF_RX_GAIN_TABLE_REGION_0_END_HZ) {
        return 0;
    }
    if (frequency <= BLADERF_RX_GAIN_TABLE_REGION_1_END_HZ) {
        return 1;
    }
    return 2;
}

/* Fill the outcome fields of a SUCCESS record from what NIOS observed.
 * Certification needs all of: a valid post-recall timestamp, the RFPLL lock
 * bit read after it, no NIOS LO readback disagreeing with the request by
 * more than 1 kHz, and a settle window that does not overflow. */
static inline void bladerf2_rx_sweep_record_success(
    struct bladerf_rx_sweep_record *record, uint64_t readback_frequency_hz,
    uint64_t executed_timestamp, uint8_t execution_flags,
    uint64_t settle_samples)
{
    const uint64_t delta =
        record->frequency > readback_frequency_hz
            ? record->frequency - readback_frequency_hz
            : readback_frequency_hz - record->frequency;

    record->status = BLADERF_RX_SWEEP_SUCCESS;
    record->flags = 0;
    record->failure_stage = 0;
    record->readback_frequency = readback_frequency_hz;
    record->executed_timestamp = 0;
    record->certified_start = 0;
    if ((execution_flags & NIOS_PKT_RETUNE2_EXEC_TS_VALID) != 0) {
        record->flags |= BLADERF_RX_SWEEP_F_EXECUTED_VALID;
        record->executed_timestamp = executed_timestamp;
    }
    if ((execution_flags & NIOS_PKT_RETUNE2_EXEC_PLL_LOCKED) != 0) {
        record->flags |= BLADERF_RX_SWEEP_F_PLL_LOCKED;
    }
    if (readback_frequency_hz != 0 && delta > 1000) {
        record->flags |= BLADERF_RX_SWEEP_F_READBACK_MISMATCH;
    }
    if ((record->flags & (BLADERF_RX_SWEEP_F_EXECUTED_VALID |
                          BLADERF_RX_SWEEP_F_PLL_LOCKED |
                          BLADERF_RX_SWEEP_F_READBACK_MISMATCH)) ==
            (BLADERF_RX_SWEEP_F_EXECUTED_VALID |
             BLADERF_RX_SWEEP_F_PLL_LOCKED) &&
        executed_timestamp <= UINT64_MAX - settle_samples) {
        record->flags |= BLADERF_RX_SWEEP_F_CERTIFIED;
        record->certified_start = executed_timestamp + settle_samples;
    }
}

/* Certification rule of bladerf_rx_sweep_label(). A sample at `timestamp`
 * belongs to the last record k scheduled at or before it. It is certified
 * at record k's LO iff record k is certified, timestamp >= its
 * certified_start, and record k + 1 exists, directly follows k in sequence
 * and is scheduled after the sample. Without k + 1 the end of k's LO is not
 * known yet; a sequence gap means a request whose schedule is unknown. The
 * checks are local to k and k + 1, so unsorted input can only lose
 * certification, never grant it wrongly. */
static inline bool bladerf2_rx_sweep_label(
    const struct bladerf_rx_sweep_record *records, unsigned int count,
    uint64_t timestamp, unsigned int *index)
{
    unsigned int lo = 0;
    unsigned int hi = count;
    unsigned int k;

    if (records == NULL || count < 2) {
        return false;
    }

    /* First record scheduled after timestamp; k is the one before it. */
    while (lo < hi) {
        const unsigned int mid = lo + (hi - lo) / 2;
        if (records[mid].scheduled_timestamp <= timestamp) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo == 0 || lo >= count) {
        return false;
    }
    k = lo - 1;

    if (records[k].status != BLADERF_RX_SWEEP_SUCCESS ||
        (records[k].flags & BLADERF_RX_SWEEP_F_CERTIFIED) == 0 ||
        timestamp < records[k].certified_start ||
        records[k].scheduled_timestamp > timestamp ||
        records[k + 1].sequence != records[k].sequence + 1u ||
        timestamp >= records[k + 1].scheduled_timestamp) {
        return false;
    }
    if (index != NULL) {
        *index = k;
    }
    return true;
}

#endif
