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

#include <libbladeRF.h>

#include "bladeRF.h"
#include "board/board.h"
#include "common.h"
#include "backend/usb/nios_access.h"
#include "helpers/have_cap.h"
#include "rf_transition_policy.h"
#include "rx_sweep_policy.h"

#if BLADERF_RX_SWEEP_QUEUE_DEPTH != NIOS_PKT_8x32_RETUNE2_COMPLETION_DEPTH
#error "BLADERF_RX_SWEEP_QUEUE_DEPTH must equal the NIOS completion depth"
#endif

#if (BLADERF2_RX_SWEEP_RING_SIZE & (BLADERF2_RX_SWEEP_RING_SIZE - 1)) != 0
#error "BLADERF2_RX_SWEEP_RING_SIZE must be a power of two"
#endif

#define SWEEP_DEPTH NIOS_PKT_8x32_RETUNE2_COMPLETION_DEPTH

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

/* Find and retire the ledger entry that a NIOS record identifies.
 * Caller holds rx_async_epoch_lock. */
static bool sweep_ledger_take(struct bladerf2_board_data *board_data,
                              uint64_t scheduled_timestamp,
                              uint16_t nios_profile,
                              uint32_t session_generation,
                              uint16_t request_token, uint8_t status,
                              struct bladerf_rx_sweep_record *record)
{
    unsigned int i;

    for (i = 0; i < SWEEP_DEPTH; i++) {
        if (board_data->rx_retune2_pending[i].valid &&
            board_data->rx_retune2_pending[i].sweep_sequence != 0 &&
            bladerf2_retune_failure_matches(
                board_data->rx_retune2_pending[i].timestamp,
                board_data->rx_retune2_pending[i].nios_profile,
                board_data->rx_retune2_pending[i].session_generation,
                board_data->rx_retune2_pending[i].request_token,
                scheduled_timestamp, nios_profile, session_generation,
                request_token)) {
            sweep_record_from_ledger(board_data, i, status, record);
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

    status = nios_retune2_success_count_read(dev, BLADERF_MODULE_RX,
                                             &success_count);
    if (status == 0) {
        status = nios_retune2_failure_count_read(dev, BLADERF_MODULE_RX,
                                                 &failure_count);
    }
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
        uint64_t readback_frequency_hz;
        uint64_t scheduled_timestamp;
        uint64_t executed_timestamp;
        uint16_t nios_profile;
        uint32_t session_generation;
        uint16_t request_token;
        uint8_t execution_flags;
        bool found;

        status = nios_retune2_completion_execution_read(
            dev, BLADERF_MODULE_RX, success_last + 1u + i,
            &readback_frequency_hz, &scheduled_timestamp, &nios_profile,
            &session_generation, &request_token, &executed_timestamp,
            &execution_flags);
        if (status != 0) {
            lost = true;
            break;
        }
        MUTEX_LOCK(&board_data->rx_async_epoch_lock);
        found = sweep_ledger_take(board_data, scheduled_timestamp,
                                  nios_profile, session_generation,
                                  request_token, BLADERF_RX_SWEEP_SUCCESS,
                                  &batch[batch_count]);
        MUTEX_UNLOCK(&board_data->rx_async_epoch_lock);
        if (!found) {
            lost = true;
            break;
        }
        bladerf2_rx_sweep_record_success(
            &batch[batch_count], readback_frequency_hz, executed_timestamp,
            execution_flags, board_data->rx_sweep.settle_samples);
        batch_count++;
    }

    for (i = 0; i < new_failures && !lost; i++) {
        uint64_t scheduled_timestamp;
        uint16_t nios_profile;
        uint8_t failure_stage;
        uint32_t session_generation;
        uint16_t request_token;
        bool found;

        status = nios_retune2_failure_completion_read(
            dev, BLADERF_MODULE_RX, failure_last + 1u + i,
            &scheduled_timestamp, &nios_profile, &failure_stage,
            &session_generation, &request_token);
        if (status != 0) {
            lost = true;
            break;
        }
        MUTEX_LOCK(&board_data->rx_async_epoch_lock);
        found = sweep_ledger_take(board_data, scheduled_timestamp,
                                  nios_profile, session_generation,
                                  request_token, BLADERF_RX_SWEEP_FAILED,
                                  &batch[batch_count]);
        MUTEX_UNLOCK(&board_data->rx_async_epoch_lock);
        if (!found) {
            lost = true;
            break;
        }
        batch[batch_count].failure_stage = failure_stage;
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
        sweep_push(board_data, &batch[i]);
    }
    return 0;
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
    bool contract_enabled;
    bool pending;
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
    pending = board_data->rx_retune2_pending_count != 0 ||
              board_data->rx_retune2_outcome_unknown;
    MUTEX_UNLOCK(&board_data->rx_async_epoch_lock);
    /* An epoch contract fences IQ in the FPGA gate on every RX retune;
     * keeping that stream open would label swept samples with a certified
     * single-LO epoch. */
    if (contract_enabled) {
        return BLADERF_ERR_UNSUPPORTED;
    }
    if (pending) {
        return BLADERF_ERR_WOULD_BLOCK;
    }

    status = nios_retune2_success_count_read(dev, BLADERF_MODULE_RX,
                                             &success_count);
    if (status == 0) {
        status = nios_retune2_failure_count_read(dev, BLADERF_MODULE_RX,
                                                 &failure_count);
    }
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
    board_data->rx_sweep.next_sequence = 1;
    board_data->rx_sweep.ring_head = 0;
    board_data->rx_sweep.ring_count = 0;
    board_data->rx_sweep.active = true;
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

    /* Free ledger slots of completed retunes before taking a new one. */
    status = sweep_collect(dev);
    if (status != 0) {
        return status;
    }

    candidate = board_data->rx_sweep.next_sequence;
    status = bladerf2_schedule_retune_sequenced(dev, ch, timestamp, frequency,
                                                quick_tune, candidate);

    /* The sequence is consumed whenever a ledger entry carries it: also for
     * an ambiguous transport failure, whose NIOS record may still arrive. */
    MUTEX_LOCK(&board_data->rx_async_epoch_lock);
    for (i = 0; i < SWEEP_DEPTH; i++) {
        if (board_data->rx_retune2_pending[i].valid &&
            board_data->rx_retune2_pending[i].sweep_sequence == candidate) {
            committed = true;
            break;
        }
    }
    MUTEX_UNLOCK(&board_data->rx_async_epoch_lock);
    if (committed) {
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
        }
    }
    MUTEX_UNLOCK(&board_data->rx_async_epoch_lock);

    status = dev->board->cancel_scheduled_retunes(dev, ch);
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
    /* NIOS recalled profiles outside the host clock driver. */
    ad9361_fastlock_invalidate_current_lo(board_data->phy, false);
    return 0;
}
