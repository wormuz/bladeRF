/* This file is part of the bladeRF project:
 *   http://www.github.com/nuand/bladeRF
 *
 * Copyright (c) 2018 Nuand LLC
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

#include "devices.h"

#if defined(BOARD_BLADERF_MICRO) && defined(BLADERF_NIOS_LIBAD936X)

#include <string.h>

#include "devices_rfic.h"


uint8_t rfic_enqueue(struct rfic_queue *q,
                     uint8_t ch,
                     uint8_t cmd,
                     uint64_t value,
                     uint16_t transaction_id)
{
    if (q->count >= COMMAND_QUEUE_MAX) {
        return COMMAND_QUEUE_FULL;
    }

    q->entries[q->ins_idx].ch    = ch;
    q->entries[q->ins_idx].cmd   = cmd;
    q->entries[q->ins_idx].transaction_id = transaction_id;
    q->entries[q->ins_idx].value = value;
    q->entries[q->ins_idx].state = ENTRY_STATE_NEW;
    q->entries[q->ins_idx].rv    = 0xFF;
    q->entries[q->ins_idx].result_code = BLADERF_RFIC_RESULT_NO_RESULT;
    q->entries[q->ins_idx].cleanup_result_code = BLADERF_RFIC_RESULT_SUCCESS;

    q->ins_idx = (q->ins_idx + 1) & (COMMAND_QUEUE_MAX - 1);

    return ++q->count;
}

uint8_t rfic_dequeue(struct rfic_queue *q, struct rfic_queue_entry *e)
{
    if (0 == q->count) {
        return COMMAND_QUEUE_EMPTY;
    }

    if (e != NULL) {
        memcpy(e, &q->entries[q->rem_idx], sizeof(*e));
    }

    q->rem_idx = (q->rem_idx + 1) & (COMMAND_QUEUE_MAX - 1);

    return --q->count;
}

void rfic_queue_progress_tick(struct rfic_queue *q)
{
    struct rfic_queue_entry *active = rfic_queue_peek(q);

    if (active != NULL && active->state == ENTRY_STATE_RUNNING) {
        q->progress_sequence++;
    }
}

struct rfic_queue_entry *rfic_queue_peek(struct rfic_queue *q)
{
    if (0 == q->count) {
        return NULL;
    } else {
        return &q->entries[q->rem_idx];
    }
}

void rfic_queue_reset(struct rfic_queue *q)
{
    q->count = 0;

    for (size_t i = 0; i < COMMAND_QUEUE_MAX; i++) {
        q->entries[i].state = ENTRY_STATE_INVALID;
    }

    q->last_rv = 0xFF;
    q->last_rv_valid = false;
    q->last_rv_sequence = 0;
    q->last_rv_transaction_id = 0;
    q->last_rv_ch = 0;
    q->last_rv_cmd = 0;
    q->last_rv_value = 0;
    q->last_result_code = BLADERF_RFIC_RESULT_NO_RESULT;
    q->last_cleanup_result_code = BLADERF_RFIC_RESULT_NO_RESULT;
    q->last_acked_transaction_id = 0;
    q->last_acked_sequence = 0;
    q->tagged_protocol_enabled = false;
    q->progress_sequence = 0;
    q->rem_idx = 0;
    q->ins_idx = 0;
}

#endif  // defined(BOARD_BLADERF_MICRO) && defined(BLADERF_NIOS_LIBAD936X)
