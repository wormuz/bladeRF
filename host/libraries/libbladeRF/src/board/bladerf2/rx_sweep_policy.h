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
    return nios_pkt_retune2_gain_table_region(frequency);
}

typedef char bladerf2_rx_gain_table_regions_match[
    BLADERF_RX_GAIN_TABLE_REGION_0_END_HZ ==
            NIOS_PKT_RETUNE2_GT_REGION_0_END_HZ &&
        BLADERF_RX_GAIN_TABLE_REGION_1_END_HZ ==
            NIOS_PKT_RETUNE2_GT_REGION_1_END_HZ ? 1 : -1];

/* RFPLL_MODULUS of ad9361.h; rx_sweep.c checks that they agree. */
#define BLADERF2_RFPLL_MODULUS 8388593u

/* Setup bits compared: selected profile <7:5>, pin select <1>, enable <0>. */
#define BLADERF2_FASTLOCK_SETUP_MASK 0xe3u

/* What NIOS read back after the recall, as the host expects it from the
 * words it uploaded to the NIOS slot: profile words 0-4 and 12 and the
 * setup register selecting AD9361 slot `rffe_profile` with fastlock enabled
 * (adi_fastlock_recall_start). */
static inline void bladerf2_rx_sweep_synth_expected(
    const uint8_t profile_words[16], uint8_t rffe_profile,
    uint8_t expected[NIOS_PKT_RETUNE2_SYNTH_LEN])
{
    unsigned int i;

    for (i = 0; i < NIOS_PKT_RETUNE2_SYNTH_VCO_DIV; i++) {
        expected[i] = profile_words[i];
    }
    expected[NIOS_PKT_RETUNE2_SYNTH_VCO_DIV] = profile_words[12];
    expected[NIOS_PKT_RETUNE2_SYNTH_SETUP] =
        (uint8_t)(((rffe_profile & 0x7u) << 5) | 0x1u);
}

/* LO of the synthesizer bytes, decoded as ad9361_fastlock_get_freq() does
 * with the RFPLL reference rate `refclk_hz`. */
static inline uint64_t bladerf2_rx_sweep_synth_frequency(
    const uint8_t synth[NIOS_PKT_RETUNE2_SYNTH_LEN], uint32_t refclk_hz)
{
    const uint64_t fract = ((uint64_t)(synth[4] & 0x7fu) << 16) |
                           ((uint64_t)synth[3] << 8) | synth[2];
    const uint64_t integer = ((uint64_t)(synth[1] & 0x7u) << 8) | synth[0];
    const uint64_t rate = (uint64_t)refclk_hz * fract /
                              BLADERF2_RFPLL_MODULUS +
                          (uint64_t)refclk_hz * integer;

    return rate >> ((synth[NIOS_PKT_RETUNE2_SYNTH_VCO_DIV] & 0xfu) + 1u);
}

enum bladerf2_rx_sweep_readback {
    BLADERF2_RX_SWEEP_READBACK_NONE,     /* no LO readback to certify by */
    BLADERF2_RX_SWEEP_READBACK_VALID,    /* *readback_hz is the read LO */
    BLADERF2_RX_SWEEP_READBACK_MISMATCH, /* NIOS read other words */
};

/* LO readback of one success record. In HOST tuning mode NIOS reports the
 * synthesizer bytes it read (`synth`, NULL when the FPGA has no word 3 or
 * the host has no expected bytes) and the host decodes them; a failed NIOS
 * read (zero setup byte) or an unknown reference rate leaves nothing to
 * certify by. In FPGA tuning mode the NIOS AD9361 driver reports the LO in
 * Hz itself (`nios_readback_hz`, 0 when not read). */
static inline enum bladerf2_rx_sweep_readback bladerf2_rx_sweep_readback(
    const uint8_t *synth, const uint8_t *expected, uint32_t refclk_hz,
    uint64_t nios_readback_hz, uint64_t *readback_hz)
{
    unsigned int i;

    *readback_hz = 0;
    if (synth != NULL && expected != NULL &&
        synth[NIOS_PKT_RETUNE2_SYNTH_SETUP] != 0) {
        if (refclk_hz != 0) {
            *readback_hz =
                bladerf2_rx_sweep_synth_frequency(synth, refclk_hz);
        }
        for (i = 0; i < NIOS_PKT_RETUNE2_SYNTH_SETUP; i++) {
            if (synth[i] != expected[i]) {
                return BLADERF2_RX_SWEEP_READBACK_MISMATCH;
            }
        }
        if ((synth[NIOS_PKT_RETUNE2_SYNTH_SETUP] &
             BLADERF2_FASTLOCK_SETUP_MASK) !=
            expected[NIOS_PKT_RETUNE2_SYNTH_SETUP]) {
            return BLADERF2_RX_SWEEP_READBACK_MISMATCH;
        }
        return refclk_hz != 0 ? BLADERF2_RX_SWEEP_READBACK_VALID
                              : BLADERF2_RX_SWEEP_READBACK_NONE;
    }
    if (nios_readback_hz != 0) {
        *readback_hz = nios_readback_hz;
        return BLADERF2_RX_SWEEP_READBACK_VALID;
    }
    return BLADERF2_RX_SWEEP_READBACK_NONE;
}

/* Fill the outcome fields of a SUCCESS record from what NIOS observed.
 * Certification needs all of: a valid post-recall timestamp, the RFPLL lock
 * bit read after it, a valid LO readback within 1 kHz of the request, and a
 * settle window that does not overflow. Without a readback the record is
 * not certified. With `gain_table_required` (the request carried a
 * gain-table byte) it also needs the RX gain table NIOS reports loaded to be
 * that of the requested LO's region. */
static inline void bladerf2_rx_sweep_record_success(
    struct bladerf_rx_sweep_record *record,
    enum bladerf2_rx_sweep_readback readback, uint64_t readback_frequency_hz,
    uint64_t executed_timestamp, uint8_t execution_flags,
    uint64_t settle_samples, bool gain_table_required)
{
    const uint64_t delta =
        record->frequency > readback_frequency_hz
            ? record->frequency - readback_frequency_hz
            : readback_frequency_hz - record->frequency;
    const uint8_t required = BLADERF_RX_SWEEP_F_EXECUTED_VALID |
                             BLADERF_RX_SWEEP_F_PLL_LOCKED |
                             BLADERF_RX_SWEEP_F_READBACK_VALID |
                             (gain_table_required
                                  ? BLADERF_RX_SWEEP_F_GAIN_TABLE_VALID
                                  : 0u);

    record->status = BLADERF_RX_SWEEP_SUCCESS;
    record->flags = 0;
    record->failure_stage = 0;
    record->gain_table = 0;
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
    if ((execution_flags & NIOS_PKT_RETUNE2_EXEC_GT_VALID) != 0) {
        record->gain_table =
            (uint8_t)NIOS_PKT_RETUNE2_EXEC_GT_REGION(execution_flags);
        if (record->gain_table ==
            bladerf2_rx_gain_table_region(record->frequency)) {
            record->flags |= BLADERF_RX_SWEEP_F_GAIN_TABLE_VALID;
        }
        if ((execution_flags & NIOS_PKT_RETUNE2_EXEC_GT_LOADED) != 0) {
            record->flags |= BLADERF_RX_SWEEP_F_GAIN_TABLE_LOADED;
        }
    }
    if (readback == BLADERF2_RX_SWEEP_READBACK_VALID) {
        record->flags |= BLADERF_RX_SWEEP_F_READBACK_VALID;
        if (delta > 1000) {
            record->flags |= BLADERF_RX_SWEEP_F_READBACK_MISMATCH;
        }
    } else if (readback == BLADERF2_RX_SWEEP_READBACK_MISMATCH) {
        record->flags |= BLADERF_RX_SWEEP_F_READBACK_MISMATCH;
    }
    if ((record->flags & (required | BLADERF_RX_SWEEP_F_READBACK_MISMATCH)) ==
            required &&
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
