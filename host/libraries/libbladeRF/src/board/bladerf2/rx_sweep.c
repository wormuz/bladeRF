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

/* RX sweep sessions: scheduled RX retunes that leave the RX stream running
 * and are certified afterwards from NIOS completion records. All entry
 * points run with dev->lock held. The RX fault monitor skips RX retune
 * records while a session is active, so this file is their only reader. */

#include <string.h>
#include <time.h>

#include <libbladeRF.h>

#include "bladeRF.h"
#include "board/board.h"
#include "common.h"
#include "backend/usb/nios_access.h"
#include "helpers/have_cap.h"
#include "rx_sweep_policy.h"

#include "ad9361_util.h"

#if BLADERF_RX_SWEEP_QUEUE_DEPTH != NIOS_PKT_8x32_RETUNE2_COMPLETION_DEPTH
#error "BLADERF_RX_SWEEP_QUEUE_DEPTH must equal the NIOS completion depth"
#endif

#if (BLADERF2_RX_SWEEP_RING_SIZE & (BLADERF2_RX_SWEEP_RING_SIZE - 1)) != 0
#error "BLADERF2_RX_SWEEP_RING_SIZE must be a power of two"
#endif

#define SWEEP_DEPTH NIOS_PKT_8x32_RETUNE2_COMPLETION_DEPTH

typedef char sweep_rfpll_modulus_matches_ad9361[
    BLADERF2_RFPLL_MODULUS == RFPLL_MODULUS ? 1 : -1];

static uint64_t sweep_monotonic_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static bool sweep_channel_valid(bladerf_channel ch)
{
    return ch == BLADERF_CHANNEL_RX(0) || ch == BLADERF_CHANNEL_RX(1);
}

/* The oldest unread record is dropped when the ring is full; the reader
 * sees a sequence gap, which certifies nothing across it. */
static void sweep_push(struct bladerf2_board_data *board_data,
                      const struct bladerf_rx_sweep_record *record)
{
    const uint32_t mask = BLADERF2_RX_SWEEP_RING_SIZE - 1u;

    if (board_data->rx_sweep.ring_count == BLADERF2_RX_SWEEP_RING_SIZE) {
        board_data->rx_sweep.ring_head =
            (board_data->rx_sweep.ring_head + 1u) & mask;
        board_data->rx_sweep.ring_count--;
    }
    board_data->rx_sweep.ring[(board_data->rx_sweep.ring_head +
                               board_data->rx_sweep.ring_count) & mask] =
        *record;
    board_data->rx_sweep.ring_count++;
}

/* The RX gain table after `record`, records taken in sequence order: what a
 * success reports, unknown after any other outcome (a failed crossing may
 * have left the table half written). Without NIOS gain-table support the
 * session never leaves its region and the table never changes. */
static void sweep_gain_table_follow(struct bladerf2_board_data *board_data,
                                    const struct bladerf_rx_sweep_record *record)
{
    if (!have_cap(board_data->capabilities, BLADERF_CAP_RETUNE2_GAIN_TABLE)) {
        return;
    }
    board_data->rx_sweep.gain_table_known =
        record->status == BLADERF_RX_SWEEP_SUCCESS &&
                (record->flags & BLADERF_RX_SWEEP_F_GAIN_TABLE_VALID) != 0
            ? record->gain_table
            : NIOS_PKT_RETUNE2_GT_UNKNOWN;
}

static void sweep_sort(struct bladerf_rx_sweep_record *records,
                       unsigned int count)
{
    unsigned int i;

    for (i = 1; i < count; i++) {
        struct bladerf_rx_sweep_record key = records[i];
        unsigned int j = i;

        while (j > 0 && records[j - 1].sequence > key.sequence) {
            records[j] = records[j - 1];
            j--;
        }
        records[j] = key;
    }
}

static void sweep_record_from_ledger(
    struct bladerf2_board_data *board_data, unsigned int slot,
    uint8_t status, struct bladerf_rx_sweep_record *record)
{
    memset(record, 0, sizeof(*record));
    record->sequence = board_data->rx_retune2_pending[slot].sweep_sequence;
    record->status = status;
    record->nios_profile = board_data->rx_retune2_pending[slot].nios_profile;
    record->frequency =
        board_data->rx_retune2_pending[slot].requested_frequency_hz;
    record->scheduled_timestamp =
        board_data->rx_retune2_pending[slot].timestamp;
}

/* Find and retire the ledger entry that a NIOS record identifies. The
 * compact record carries the request token (unique among pending requests,
 * nonzero), the NIOS profile and the low byte of the command session; the
 * scheduled timestamp and the expected synthesizer bytes come from the
 * ledger. Caller holds rx_async_epoch_lock. */
static bool sweep_ledger_take(struct bladerf2_board_data *board_data,
                              const struct nios_retune2_record *nios,
                              uint8_t status,
                              struct bladerf_rx_sweep_record *record,
                              uint8_t *synth_expected,
                              bool *synth_expected_valid,
                              uint8_t *gain_table)
{
    unsigned int i;

    for (i = 0; i < SWEEP_DEPTH; i++) {
        if (board_data->rx_retune2_pending[i].valid &&
            board_data->rx_retune2_pending[i].sweep_sequence != 0 &&
            board_data->rx_retune2_pending[i].request_token != 0 &&
            board_data->rx_retune2_pending[i].request_token ==
                nios->request_token &&
            board_data->rx_retune2_pending[i].nios_profile ==
                nios->nios_profile &&
            (uint8_t)board_data->rx_retune2_pending[i].session_generation ==
                nios->session_generation_low) {
            sweep_record_from_ledger(board_data, i, status, record);
            if (synth_expected != NULL) {
                memcpy(synth_expected,
                       board_data->rx_retune2_pending[i].synth_expected,
                       NIOS_PKT_RETUNE2_SYNTH_LEN);
                *synth_expected_valid =
                    board_data->rx_retune2_pending[i].synth_expected_valid;
            }
            if (gain_table != NULL) {
                *gain_table = board_data->rx_retune2_pending[i].gain_table;
            }
            board_data->rx_retune2_pending[i].valid = false;
            if (board_data->rx_retune2_pending_count != 0) {
                board_data->rx_retune2_pending_count--;
            }
            return true;
        }
    }
    return false;
}

/* Read every NIOS RX success and failure record since the last call and
 * turn it into a sweep record. Any record that cannot be read and matched
 * makes the outcome of every still pending request unknown: they become
 * LOST, exactly as the RX monitor treats a lost completion. */
static int sweep_collect(struct bladerf *dev)
{
    struct bladerf2_board_data *board_data = dev->board_data;
    struct bladerf_rx_sweep_record batch[2 * SWEEP_DEPTH];
    unsigned int batch_count = 0;
    uint32_t success_count = 0;
    uint32_t failure_count = 0;
    uint32_t success_last;
    uint32_t failure_last;
    uint32_t new_successes;
    uint32_t new_failures;
    bool baselines_valid;
    uint8_t pending_count;
    bool lost = false;
    uint32_t i;
    int status;
    const bool read_synth = have_cap(board_data->capabilities,
                                     BLADERF_CAP_RETUNE2_SYNTH_RECORD);

    /* One read for both counters; each success record then costs two
     * reads (three with the synthesizer word) and each failure record
     * one. */
    status = nios_retune2_record_counts(dev, BLADERF_MODULE_RX,
                                        &success_count, &failure_count);
    if (status != 0) {
        return status;
    }

    MUTEX_LOCK(&board_data->rx_async_epoch_lock);
    success_last = board_data->rx_retune2_success_count_last;
    failure_last = board_data->rx_retune2_failure_count_last;
    baselines_valid = board_data->rx_retune2_success_count_valid &&
                      board_data->rx_retune2_failure_count_valid;
    pending_count = board_data->rx_retune2_pending_count;
    MUTEX_UNLOCK(&board_data->rx_async_epoch_lock);

    new_successes = success_count - success_last;
    new_failures = failure_count - failure_last;
    if (!baselines_valid || new_successes > SWEEP_DEPTH ||
        new_failures > SWEEP_DEPTH ||
        new_successes + new_failures > pending_count) {
        lost = new_successes != 0 || new_failures != 0 || !baselines_valid;
    }

    for (i = 0; i < new_successes && !lost; i++) {
        struct nios_retune2_record nios;
        uint8_t synth_expected[NIOS_PKT_RETUNE2_SYNTH_LEN];
        bool synth_expected_valid = false;
        uint8_t gain_table = 0;
        enum bladerf2_rx_sweep_readback readback;
        uint64_t readback_hz;
        uint64_t executed_timestamp;
        bool found;

        status = nios_retune2_record_read(dev, BLADERF_MODULE_RX, false,
                                          success_last + 1u + i, read_synth,
                                          &nios);
        if (status != 0) {
            lost = true;
            break;
        }
        MUTEX_LOCK(&board_data->rx_async_epoch_lock);
        found = sweep_ledger_take(board_data, &nios, BLADERF_RX_SWEEP_SUCCESS,
                                  &batch[batch_count], synth_expected,
                                  &synth_expected_valid, &gain_table);
        MUTEX_UNLOCK(&board_data->rx_async_epoch_lock);
        if (!found) {
            lost = true;
            break;
        }
        executed_timestamp =
            batch[batch_count].scheduled_timestamp + nios.executed_delta;
        if (executed_timestamp < batch[batch_count].scheduled_timestamp) {
            nios.execution_flags &=
                (uint8_t)~NIOS_PKT_RETUNE2_EXEC_TS_VALID;
        }
        readback = bladerf2_rx_sweep_readback(
            nios.synth_read ? nios.synth : NULL,
            synth_expected_valid ? synth_expected : NULL,
            board_data->rx_sweep.refclk_hz, nios.readback_frequency_hz,
            &readback_hz);
        bladerf2_rx_sweep_record_success(
            &batch[batch_count], readback, readback_hz, executed_timestamp,
            nios.execution_flags, board_data->rx_sweep.settle_samples,
            gain_table != 0);
        batch_count++;
    }

    for (i = 0; i < new_failures && !lost; i++) {
        struct nios_retune2_record nios;
        bool found;

        status = nios_retune2_record_read(dev, BLADERF_MODULE_RX, true,
                                          failure_last + 1u + i, false, &nios);
        if (status != 0) {
            lost = true;
            break;
        }
        MUTEX_LOCK(&board_data->rx_async_epoch_lock);
        found = sweep_ledger_take(board_data, &nios, BLADERF_RX_SWEEP_FAILED,
                                  &batch[batch_count], NULL, NULL, NULL);
        MUTEX_UNLOCK(&board_data->rx_async_epoch_lock);
        if (!found) {
            lost = true;
            break;
        }
        batch[batch_count].failure_stage = nios.failure_stage;
        batch_count++;
    }

    MUTEX_LOCK(&board_data->rx_async_epoch_lock);
    if (lost) {
        for (i = 0; i < SWEEP_DEPTH; i++) {
            if (board_data->rx_retune2_pending[i].valid) {
                if (board_data->rx_retune2_pending[i].sweep_sequence != 0 &&
                    batch_count < 2 * SWEEP_DEPTH) {
                    sweep_record_from_ledger(board_data, i,
                                             BLADERF_RX_SWEEP_LOST,
                                             &batch[batch_count++]);
                }
                board_data->rx_retune2_pending[i].valid = false;
            }
        }
        board_data->rx_retune2_pending_count = 0;
        /* Same state as a lost monitor completion: no further retunes until
         * the queue is cleared (bladerf_rx_sweep_end). */
        board_data->rx_retune2_outcome_unknown = true;
    }
    board_data->rx_retune2_success_count_last = success_count;
    board_data->rx_retune2_success_count_valid = true;
    board_data->rx_retune2_failure_count_last = failure_count;
    board_data->rx_retune2_failure_count_valid = true;
    MUTEX_UNLOCK(&board_data->rx_async_epoch_lock);

    if (lost) {
        ad9361_fastlock_invalidate_current_lo(board_data->phy, false);
        log_debug("%s: RX sweep completion lost (successes %u, failures %u, "
                  "pending %u)\n", __FUNCTION__, new_successes, new_failures,
                  pending_count);
    }

    sweep_sort(batch, batch_count);
    for (i = 0; i < batch_count; i++) {
        sweep_gain_table_follow(board_data, &batch[i]);
        sweep_push(board_data, &batch[i]);
    }
    return 0;
}

/* The gain-table region the session may sweep in: that of the current LO,
 * which a slow tune or transition loaded the table for. With a host AD9361
 * handle also check that the driver's tables have the public boundaries
 * and that the loaded table is the one for this LO. */
static int sweep_gain_table_region(struct bladerf *dev, bladerf_channel ch,
                                   unsigned int *region)
{
    struct bladerf2_board_data *board_data = dev->board_data;
    const struct ad9361_rf_phy *phy = board_data->phy;
    bladerf_frequency frequency = 0;
    int status;

    status = dev->board->get_frequency(dev, ch, &frequency);
    if (status != 0) {
        return status;
    }
    *region = bladerf2_rx_gain_table_region(frequency);

    if (phy != NULL && phy->gt_info != NULL && phy->pdata != NULL) {
        unsigned int i;

        for (i = 0; phy->gt_info[i].tab != NULL; i++) {
            const struct gain_table_info *info = &phy->gt_info[i];

            if (info->split_table != phy->pdata->split_gt) {
                continue;
            }
            /* Every table must sit entirely inside one public region. */
            if (bladerf2_rx_gain_table_region(info->start + 1) !=
                bladerf2_rx_gain_table_region(info->end)) {
                return BLADERF_ERR_UNSUPPORTED;
            }
        }
        if (phy->current_table >= i ||
            bladerf2_rx_gain_table_region(
                phy->gt_info[phy->current_table].end) != *region) {
            /* The loaded table does not belong to the current LO. */
            return BLADERF_ERR_WOULD_BLOCK;
        }
    }
    return 0;
}

/* Region of the AD9361 driver's loaded RX gain table. */
static unsigned int gain_table_driver_region(const struct ad9361_rf_phy *phy)
{
    if (phy == NULL || phy->gt_info == NULL ||
        phy->current_table >= RXGAIN_TBLS_END) {
        return NIOS_PKT_RETUNE2_GT_UNKNOWN;
    }
    return bladerf2_rx_gain_table_region(
        phy->gt_info[phy->current_table].end);
}

void bladerf2_rx_gain_table_loaded(struct bladerf2_board_data *board_data,
                                   unsigned int region)
{
    struct ad9361_rf_phy *phy = board_data->phy;
    unsigned int i;

    if (phy == NULL || phy->gt_info == NULL || phy->pdata == NULL) {
        return;
    }
    phy->current_table = NO_GAIN_TABLE;
    if (region == NIOS_PKT_RETUNE2_GT_UNKNOWN) {
        return;
    }
    for (i = 0; phy->gt_info[i].tab != NULL; i++) {
        if (!!phy->gt_info[i].split_table == !!phy->pdata->split_gt &&
            bladerf2_rx_gain_table_region(phy->gt_info[i].end) == region) {
            phy->current_table = i;
            return;
        }
    }
}

/* The full-table gain bladerf_set_gain(gain_db) writes at `region`: the gain
 * minus the region's offset, clamped to its "full" stage range, as
 * get_gain_offset() and _rfic_host_set_gain_stage() do at an LO there. */
static int gain_table_full_db(unsigned int region, int gain_db)
{
    static const int64_t inside[3] = {
        INT64_C(1000000000), INT64_C(2000000000), INT64_C(5000000000),
    };
    float offset = 0.0f;
    struct bladerf_range const *full = NULL;
    int val;
    size_t i;

    for (i = 0; i < ARRAY_SIZE(bladerf2_rx_gain_ranges); i++) {
        struct bladerf_gain_range const *r = &bladerf2_rx_gain_ranges[i];

        if (inside[region] < r->frequency.min ||
            inside[region] > r->frequency.max) {
            continue;
        }
        if (r->name == NULL) {
            offset = r->offset;
        } else if (strcmp(r->name, "full") == 0) {
            full = &r->gain;
        }
    }
    val = gain_db - offset;
    if (full != NULL) {
        if (val < full->min) {
            val = (int)full->min;
        }
        if (val > full->max) {
            val = (int)full->max;
        }
    }
    return val;
}

int bladerf2_rx_gain_table_index_upload(struct bladerf *dev)
{
    struct bladerf2_board_data *board_data = dev->board_data;
    const struct ad9361_rf_phy *phy = board_data->phy;
    const struct gain_table_info *gt =
        phy != NULL && phy->gt_info != NULL ? phy->gt_info
                                            : ad9361_adi_gt_info;
    const bool split = phy != NULL && phy->pdata != NULL &&
                       phy->pdata->split_gt;
    bool known[2];
    int gain_db[2];
    unsigned int lane;
    unsigned int region;

    if (!have_cap(board_data->capabilities, BLADERF_CAP_RETUNE2_GAIN_TABLE)) {
        return 0;
    }
    MUTEX_LOCK(&board_data->rx_async_epoch_lock);
    for (lane = 0; lane < 2; lane++) {
        known[lane] = board_data->rx_gain_api_known[lane];
        gain_db[lane] = board_data->rx_gain_api_db[lane];
    }
    MUTEX_UNLOCK(&board_data->rx_async_epoch_lock);
    /* bladerf2_rx_reapply_mgc_gain(): only a manual gain the API set. */
    for (lane = 0; lane < 2; lane++) {
        bladerf_gain_mode mode;

        if (known[lane] &&
            (board_data->rfic->get_gain_mode(dev, BLADERF_CHANNEL_RX(lane),
                                             &mode) != 0 ||
             mode != BLADERF_GAIN_MGC)) {
            known[lane] = false;
        }
    }

    for (region = 0; region < 3; region++) {
        const struct gain_table_info *info = NULL;
        uint32_t indices = 0;
        unsigned int i;
        int status;

        for (i = 0; gt[i].tab != NULL; i++) {
            if (!!gt[i].split_table == split &&
                bladerf2_rx_gain_table_region(gt[i].end) == region) {
                info = &gt[i];
                break;
            }
        }
        for (lane = 0; lane < 2; lane++) {
            int32_t index = -1;

            if (known[lane] && info != NULL) {
                index = ad9361_gain_table_index(
                    info, gain_table_full_db(region, gain_db[lane]));
            }
            indices |= (uint32_t)(index < 0 ? NIOS_PKT_8x32_RX_GT_INDEX_KEEP
                                            : (uint32_t)index)
                       << (8 * lane);
        }
        status = nios_rx_gain_table_index_write(dev, (uint8_t)region,
                                                indices);
        if (status != 0) {
            return status;
        }
    }
    return 0;
}

uint8_t bladerf2_rx_retune_gain_table(struct bladerf *dev,
                                      bladerf_frequency frequency, bool sweep,
                                      bool queue_idle)
{
    struct bladerf2_board_data *board_data = dev->board_data;
    const struct ad9361_rf_phy *phy = board_data->phy;
    const unsigned int to = bladerf2_rx_gain_table_region(frequency);
    unsigned int from = NIOS_PKT_RETUNE2_GT_UNKNOWN;
    bool split = false;
    bool elna = false;

    if (!have_cap(board_data->capabilities, BLADERF_CAP_RETUNE2_GAIN_TABLE)) {
        return 0;
    }
    if (frequency == 0) {
        /* No LO to name a table for: the recall keeps whichever is loaded,
         * which may then not match it. */
        bladerf2_rx_gain_table_loaded(board_data,
                                      NIOS_PKT_RETUNE2_GT_UNKNOWN);
        return 0;
    }
    /* In FPGA tuning mode NIOS's own driver knows the table and these
     * fields are ignored. */
    if (phy != NULL && phy->pdata != NULL) {
        split = phy->pdata->split_gt;
        elna = phy->pdata->elna_ctrl.elna_in_gaintable_all_index_en;
    }
    if (sweep) {
        from = board_data->rx_sweep.gain_table_planned;
    } else if (queue_idle) {
        from = gain_table_driver_region(phy);
    }
    if (from != to) {
        /* NIOS may load another table at a time the host does not see. */
        bladerf2_rx_gain_table_loaded(board_data,
                                      NIOS_PKT_RETUNE2_GT_UNKNOWN);
    }
    return nios_pkt_retune2_gain_table(split, elna, from, to);
}

bool bladerf2_rx_sweep_active(struct bladerf *dev, bladerf_channel ch)
{
    struct bladerf2_board_data *board_data = dev->board_data;

    return board_data != NULL && sweep_channel_valid(ch) &&
           board_data->rx_sweep.active;
}

int bladerf2_rx_sweep_begin(struct bladerf *dev, bladerf_channel ch,
                            uint64_t settle_samples)
{
    struct bladerf2_board_data *board_data = dev->board_data;
    uint32_t success_count = 0;
    uint32_t failure_count = 0;
    struct bladerf_rf_event event;
    bool contract_enabled;
    bool epoch_certified;
    bool pending;
    unsigned int gain_table_region = 0;
    uint32_t refclk_hz = 0;
    int status;

    if (board_data == NULL || !sweep_channel_valid(ch)) {
        return BLADERF_ERR_INVAL;
    }
    if (!have_cap(board_data->capabilities, BLADERF_CAP_SCHEDULED_RETUNE) ||
        !have_cap(board_data->capabilities,
                  BLADERF_CAP_RETUNE2_REQUEST_TOKEN) ||
        !have_cap(board_data->capabilities,
                  BLADERF_CAP_RETUNE2_EXECUTION_RECORD)) {
        return BLADERF_ERR_UNSUPPORTED;
    }
    if (board_data->rx_sweep.active || board_data->rf_transition_pending ||
        board_data->rf_transition_setter_active) {
        return BLADERF_ERR_WOULD_BLOCK;
    }

    MUTEX_LOCK(&board_data->rx_async_epoch_lock);
    contract_enabled = board_data->rf_transition_epoch_contract_enabled;
    epoch_certified = board_data->rf_transition_epoch_certified;
    pending = board_data->rx_retune2_pending_count != 0 ||
              board_data->rx_retune2_outcome_unknown;
    MUTEX_UNLOCK(&board_data->rx_async_epoch_lock);
    /* With an epoch contract the FPGA gate (rx_epoch_gate.vhd) is fenced
     * only by host ARM/ABORT commands; a NIOS recall drives neither, so an
     * open epoch keeps forwarding real samples under one epoch ID while
     * the session retunes. The session therefore starts from an open,
     * certified epoch and suppresses the host-side revocations
     * (bladerf_schedule_retune invalidation, RX monitor LO checks) that
     * would ABORT it. A closed or pending epoch would only emit zero-IQ
     * keepalives until a transition, which a session excludes. */
    if (pending || (contract_enabled && !epoch_certified)) {
        return BLADERF_ERR_WOULD_BLOCK;
    }

    status = sweep_gain_table_region(dev, ch, &gain_table_region);
    if (status != 0) {
        return status;
    }
    /* Crossing retunes write the manual gain index for the API gain at the
     * new region, as the slow path re-applies it. */
    status = bladerf2_rx_gain_table_index_upload(dev);
    if (status != 0) {
        return status;
    }

    /* Decodes the synthesizer words NIOS reads back after each recall. */
    if (board_data->phy != NULL) {
        status = clk_get_rate_checked(board_data->phy,
                                      board_data->phy->ref_clk_scale[RX_REFCLK],
                                      &refclk_hz);
        if (status < 0) {
            return errno_ad9361_to_bladerf(status);
        }
    }

    status = nios_retune2_record_counts(dev, BLADERF_MODULE_RX,
                                        &success_count, &failure_count);
    if (status != 0) {
        return status;
    }

    MUTEX_LOCK(&board_data->rx_async_epoch_lock);
    board_data->rx_retune2_success_count_last = success_count;
    board_data->rx_retune2_success_count_valid = true;
    board_data->rx_retune2_failure_count_last = failure_count;
    board_data->rx_retune2_failure_count_valid = true;
    MUTEX_UNLOCK(&board_data->rx_async_epoch_lock);

    board_data->rx_sweep.ch = ch;
    board_data->rx_sweep.settle_samples = settle_samples;
    board_data->rx_sweep.gain_table_region = gain_table_region;
    board_data->rx_sweep.gain_table_planned = (uint8_t)gain_table_region;
    board_data->rx_sweep.gain_table_known = (uint8_t)gain_table_region;
    board_data->rx_sweep.refclk_hz = refclk_hz;
    board_data->rx_sweep.next_sequence = 1;
    board_data->rx_sweep.ring_head = 0;
    board_data->rx_sweep.ring_count = 0;
    board_data->rx_sweep.active = true;

    /* The open epoch's LO claim ends with the first sweep retune. */
    memset(&event, 0, sizeof(event));
    event.host_monotonic_ns = sweep_monotonic_ns();
    event.event_type = BLADERF_RF_EVT_RX_SWEEP_BEGIN;
    event.fpga_state = board_data->rf_transition_state;
    event.epoch_id = board_data->rf_transition_epoch_id;
    event.requested_rx_lo_hz =
        board_data->rf_transition_readback_frequency_hz;
    event.flags = contract_enabled ? 1u : 0u;
    bladerf2_rf_event_append(board_data, &event);
    return 0;
}

int bladerf2_rx_sweep_schedule(struct bladerf *dev, bladerf_channel ch,
                               bladerf_timestamp timestamp,
                               bladerf_frequency frequency,
                               const struct bladerf_quick_tune *quick_tune,
                               uint32_t *sequence)
{
    struct bladerf2_board_data *board_data = dev->board_data;
    uint32_t candidate;
    bool committed = false;
    unsigned int i;
    int status;

    if (sequence != NULL) {
        *sequence = 0;
    }
    if (board_data == NULL || !board_data->rx_sweep.active ||
        ch != board_data->rx_sweep.ch || quick_tune == NULL ||
        timestamp == BLADERF_RETUNE_NOW) {
        return BLADERF_ERR_INVAL;
    }
    /* Without NIOS gain-table loading a recall keeps the loaded table;
     * outside its region the gain would be off by several dB (measured
     * 6.4 dB at 925 MHz coming from a 2.5 GHz table). */
    if (!have_cap(board_data->capabilities, BLADERF_CAP_RETUNE2_GAIN_TABLE) &&
        bladerf2_rx_gain_table_region(frequency) !=
            board_data->rx_sweep.gain_table_region) {
        return BLADERF_ERR_RANGE;
    }

    /* Free ledger slots of completed retunes before taking a new one. */
    status = sweep_collect(dev);
    if (status != 0) {
        return status;
    }

    candidate = board_data->rx_sweep.next_sequence;
    status = bladerf2_schedule_retune_sequenced(dev, ch, timestamp, frequency,
                                                quick_tune, candidate);

    /* The sequence is consumed whenever a ledger entry carries it: also for
     * an ambiguous transport failure, whose NIOS record may still arrive.
     * The entry also takes the synthesizer bytes NIOS must read back:
     * NIOS snapshots the slot's profile at enqueue, so take the host's
     * words for the slot now, not when the record arrives. */
    MUTEX_LOCK(&board_data->rx_async_epoch_lock);
    for (i = 0; i < SWEEP_DEPTH; i++) {
        if (board_data->rx_retune2_pending[i].valid &&
            board_data->rx_retune2_pending[i].sweep_sequence == candidate) {
            /* A ledger entry exists only after
             * bladerf2_schedule_retune_sequenced() range-checked
             * nios_profile. */
            const struct bladerf2_nios_fastlock_words *words =
                &board_data->nios_fastlock_words[0][quick_tune->nios_profile];

            board_data->rx_retune2_pending[i].synth_expected_valid =
                words->valid;
            if (words->valid) {
                bladerf2_rx_sweep_synth_expected(
                    words->words, (uint8_t)quick_tune->rffe_profile,
                    board_data->rx_retune2_pending[i].synth_expected);
            }
            committed = true;
            break;
        }
    }
    MUTEX_UNLOCK(&board_data->rx_async_epoch_lock);
    if (committed) {
        /* The next request starts from this one's table; after an
         * ambiguous transport failure this one may never run, so the next
         * names no source table and NIOS loads its own. */
        board_data->rx_sweep.gain_table_planned =
            status == 0 ? (uint8_t)bladerf2_rx_gain_table_region(frequency)
                        : (uint8_t)NIOS_PKT_RETUNE2_GT_UNKNOWN;
        board_data->rx_sweep.next_sequence = candidate + 1u;
        if (sequence != NULL) {
            *sequence = candidate;
        }
    }
    return status;
}

int bladerf2_rx_sweep_completions(struct bladerf *dev, bladerf_channel ch,
                                  struct bladerf_rx_sweep_record *records,
                                  unsigned int max_records,
                                  unsigned int *count)
{
    struct bladerf2_board_data *board_data = dev->board_data;
    const uint32_t mask = BLADERF2_RX_SWEEP_RING_SIZE - 1u;
    unsigned int n = 0;

    if (count != NULL) {
        *count = 0;
    }
    if (board_data == NULL || count == NULL ||
        (records == NULL && max_records != 0) || !sweep_channel_valid(ch)) {
        return BLADERF_ERR_INVAL;
    }
    if (board_data->rx_sweep.active) {
        int status;

        if (ch != board_data->rx_sweep.ch) {
            return BLADERF_ERR_INVAL;
        }
        status = sweep_collect(dev);
        if (status != 0) {
            return status;
        }
    }

    while (n < max_records && board_data->rx_sweep.ring_count != 0) {
        records[n++] = board_data->rx_sweep.ring[board_data->rx_sweep.ring_head];
        board_data->rx_sweep.ring_head =
            (board_data->rx_sweep.ring_head + 1u) & mask;
        board_data->rx_sweep.ring_count--;
    }
    *count = n;
    return 0;
}

int bladerf2_rx_sweep_end(struct bladerf *dev, bladerf_channel ch)
{
    struct bladerf2_board_data *board_data = dev->board_data;
    struct bladerf_rx_sweep_record cancelled[SWEEP_DEPTH];
    struct bladerf_rx_sweep_record end_record;
    struct bladerf_rf_event event;
    unsigned int cancelled_count = 0;
    bladerf_timestamp end_timestamp = 0;
    bool end_timestamp_valid;
    unsigned int i;
    int status;

    if (board_data == NULL || !board_data->rx_sweep.active ||
        ch != board_data->rx_sweep.ch) {
        return BLADERF_ERR_INVAL;
    }

    /* Best effort: a failed read only turns more requests into CANCELLED. */
    (void)sweep_collect(dev);

    MUTEX_LOCK(&board_data->rx_async_epoch_lock);
    for (i = 0; i < SWEEP_DEPTH; i++) {
        if (board_data->rx_retune2_pending[i].valid &&
            board_data->rx_retune2_pending[i].sweep_sequence != 0) {
            sweep_record_from_ledger(board_data, i,
                                     BLADERF_RX_SWEEP_CANCELLED,
                                     &cancelled[cancelled_count++]);
            /* It may or may not have run; if it may load a table, which
             * one is loaded is unknown. */
            if (NIOS_PKT_RETUNE2_GT_FROM(
                    board_data->rx_retune2_pending[i].gain_table) !=
                NIOS_PKT_RETUNE2_GT_TO(
                    board_data->rx_retune2_pending[i].gain_table)) {
                board_data->rx_sweep.gain_table_known =
                    NIOS_PKT_RETUNE2_GT_UNKNOWN;
            }
        }
    }
    MUTEX_UNLOCK(&board_data->rx_async_epoch_lock);

    /* The session owns this queue the way a transition does; the public
     * cancel refuses while an epoch contract is active. */
    board_data->rf_transition_cancelling_scheduled_retunes = true;
    status = dev->board->cancel_scheduled_retunes(dev, ch);
    board_data->rf_transition_cancelling_scheduled_retunes = false;
    if (status != 0) {
        /* The queue may still run; keep the session and its ledger. */
        return status;
    }

    end_timestamp_valid =
        dev->board->get_timestamp(dev, BLADERF_RX, &end_timestamp) == 0;
    if (!end_timestamp_valid) {
        /* Zero precedes every record: the last range then ends before it
         * starts, so nothing past the last schedule is certified. */
        end_timestamp = 0;
    }

    sweep_sort(cancelled, cancelled_count);
    for (i = 0; i < cancelled_count; i++) {
        /* Requests due after the clear never ran; the session ends at
         * end_timestamp either way, so no range may extend past it. */
        if (cancelled[i].scheduled_timestamp > end_timestamp) {
            cancelled[i].scheduled_timestamp = end_timestamp;
        }
        sweep_push(board_data, &cancelled[i]);
    }

    memset(&end_record, 0, sizeof(end_record));
    end_record.sequence = board_data->rx_sweep.next_sequence;
    end_record.status = BLADERF_RX_SWEEP_END;
    end_record.scheduled_timestamp = end_timestamp;
    sweep_push(board_data, &end_record);

    board_data->rx_sweep.active = false;
    /* NIOS recalled profiles outside the host clock driver, and loaded RX
     * gain tables outside the AD9361 driver: hand it the one loaded now. */
    if (have_cap(board_data->capabilities, BLADERF_CAP_RETUNE2_GAIN_TABLE)) {
        bladerf2_rx_gain_table_loaded(board_data,
                                      board_data->rx_sweep.gain_table_known);
    }
    ad9361_fastlock_invalidate_current_lo(board_data->phy, false);

    /* The caller (bladerf_rx_sweep_end) then revokes the epoch: its LO is
     * no longer the one the transition certified. */
    memset(&event, 0, sizeof(event));
    event.host_monotonic_ns = sweep_monotonic_ns();
    event.fpga_timestamp = end_timestamp;
    event.event_type = BLADERF_RF_EVT_RX_SWEEP_END;
    event.fpga_state = board_data->rf_transition_state;
    event.epoch_id = board_data->rf_transition_epoch_id;
    event.flags = end_record.sequence;
    bladerf2_rf_event_append(board_data, &event);
    return 0;
}
