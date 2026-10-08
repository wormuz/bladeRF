/*
 * Copyright (C) 2014-2015 Nuand LLC
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 */

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* Only switch on the verbose debug prints in this file when we *really* want
 * them. Otherwise, compile them out to avoid excessive log level checks
 * in our data path */
#include "log.h"
#ifndef ENABLE_LIBBLADERF_SYNC_LOG_VERBOSE
#undef log_verbose
#define log_verbose(...)
#endif
#include "rel_assert.h"
#include "conversions.h"
#include "minmax.h"

#include "async.h"
#include "sync.h"
#include "sync_worker.h"

#include "board/board.h"
#include "backend/usb/usb.h"

#define worker2str(s) (direction2str(s->stream_config.layout & BLADERF_DIRECTION_MASK))

unsigned int sync_worker_transfer_timeout_ms(bladerf_direction direction,
                                             bladerf_format format,
                                             unsigned int configured_ms)
{
    unsigned int timeout_ms = uint_max(configured_ms, BULK_TIMEOUT_MS);

    if (direction == BLADERF_RX && format == BLADERF_FORMAT_SC16_Q11_META) {
        timeout_ms = uint_max(timeout_ms,
                              SYNC_RX_META_TRANSITION_TIMEOUT_MS);
    }

    return timeout_ms;
}

void *sync_worker_task(void *arg);

static void mark_buffer_ready(struct buffer_mgmt *b,
                              unsigned int idx,
                              size_t num_samples)
{
    b->status[idx]         = SYNC_BUFFER_FULL;
    b->actual_lengths[idx] = num_samples;
    COND_SIGNAL(&b->buf_ready);
}

static void flush_reorder_entries(struct buffer_mgmt *b)
{
    bool progressed = true;

    while (progressed && b->reorder_len > 0) {
        progressed = false;

        for (size_t i = 0; i < b->reorder_len; ++i) {
            if (b->reorder[i].seq == b->expected_seq) {
                if (!b->reorder[i].dropped) {
                    mark_buffer_ready(b, b->reorder[i].buf_idx,
                                      b->reorder[i].num_samples);
                }
                b->expected_seq++;

                for (size_t j = i; j + 1 < b->reorder_len; ++j) {
                    b->reorder[j] = b->reorder[j + 1];
                }

                b->reorder_len--;
                progressed = true;
                break;
            }
        }
    }
}

static bool hold_out_of_order_buffer(struct buffer_mgmt *b,
                                     uint32_t seq,
                                     unsigned int idx,
                                     size_t num_samples,
                                     bool dropped)
{
    if (b->reorder_limit == 0 || b->reorder_len >= b->reorder_limit) {
        return false;
    }

    const uint32_t new_distance = seq - b->expected_seq;
    size_t insert_pos           = 0;

    while (insert_pos < b->reorder_len) {
        const uint32_t existing_distance =
            b->reorder[insert_pos].seq - b->expected_seq;

        if (existing_distance > new_distance) {
            break;
        }

        insert_pos++;
    }

    for (size_t i = b->reorder_len; i > insert_pos; --i) {
        b->reorder[i] = b->reorder[i - 1];
    }

    b->reorder[insert_pos].seq         = seq;
    b->reorder[insert_pos].buf_idx     = idx;
    b->reorder[insert_pos].num_samples = num_samples;
    b->reorder[insert_pos].dropped     = dropped;
    b->reorder_len++;

    return true;
}

static bool hold_dropped_sequence(struct buffer_mgmt *b, uint32_t seq)
{
    if (b->reorder_len >= SYNC_RX_MAX_REORDER) {
        return false;
    }

    const uint32_t distance = seq - b->expected_seq;
    size_t insert_pos = 0;
    while (insert_pos < b->reorder_len &&
           b->reorder[insert_pos].seq - b->expected_seq <= distance) {
        insert_pos++;
    }

    for (size_t i = b->reorder_len; i > insert_pos; --i) {
        b->reorder[i] = b->reorder[i - 1];
    }

    b->reorder[insert_pos].seq = seq;
    b->reorder[insert_pos].buf_idx = BUFFER_MGMT_INVALID_INDEX;
    b->reorder[insert_pos].num_samples = 0;
    b->reorder[insert_pos].dropped = true;
    b->reorder_len++;
    return true;
}

static void note_rx_overrun(struct bladerf_sync *s, uint32_t source_flags);

static void retire_dropped_sequence(struct bladerf_sync *s, uint32_t seq)
{
    struct buffer_mgmt *b = &s->buf_mgmt;
    if (seq == b->expected_seq) {
        b->expected_seq++;
        flush_reorder_entries(b);
    } else if (seq - b->expected_seq != 0 &&
               !hold_dropped_sequence(b, seq)) {
        log_warning("RX dropped-sequence queue full: seq=%u expected=%u\n",
                    seq, b->expected_seq);
        /* Losing a tombstone leaves an unretirable sequence hole. Surface a
         * discontinuity and wake sync_rx so RX_NOW can discard stale ring
         * contents and the META parser can re-establish timestamp continuity.
         * Never treat queue overflow as permission to admit reordered IQ. */
        note_rx_overrun(s, BLADERF_RF_STREAM_STATUS_SYNC_RX_QUEUE |
                           BLADERF_RF_STREAM_STATUS_SYNC_RX_SEQUENCE_TRACKER);
    }
}

void sync_worker_rx_ring_full_recycle(struct bladerf_sync *s,
                                      unsigned int buffer_idx)
{
    struct buffer_mgmt *b;

    if (s == NULL || !s->initialized) {
        return;
    }

    b = &s->buf_mgmt;
    if (buffer_idx >= b->num_buffers) {
        return;
    }

    if (b->buffer_seq != NULL) {
        retire_dropped_sequence(s, b->buffer_seq[buffer_idx]);
    }

    /* This callback returns the same completed USB buffer for reuse. Unlike
     * an epoch-rejected EMPTY slot, it is not left as a dropped ring entry:
     * the slot immediately represents the new in-flight sequence. */
    sync_worker_mark_rx_slot_in_flight(b, buffer_idx, b->next_seq++);
}

unsigned int sync_worker_rx_select_producer_slot(struct bladerf_sync *s)
{
    struct buffer_mgmt *b;
    unsigned int idx;

    if (s == NULL || !s->initialized) {
        return BUFFER_MGMT_INVALID_INDEX;
    }

    b = &s->buf_mgmt;
    if (b->status == NULL || b->num_buffers == 0 ||
        b->prod_i >= b->num_buffers) {
        return BUFFER_MGMT_INVALID_INDEX;
    }

    idx = b->prod_i;
    for (unsigned int offset = 0; offset < b->num_buffers; ++offset) {
        idx = (b->prod_i + offset) % b->num_buffers;
        if (b->status[idx] == SYNC_BUFFER_EMPTY) {
            if (idx != b->prod_i) {
                log_warning("RX sync producer cursor repair: buffers=%u "
                            "old_prod=%u old_state=%u empty_slot=%u "
                            "cons=%u expected_seq=%u next_seq=%u\n",
                            b->num_buffers, b->prod_i,
                            (unsigned)b->status[b->prod_i], idx, b->cons_i,
                            b->expected_seq, b->next_seq);
                b->prod_i = idx;
            }
            return idx;
        }
    }

    return BUFFER_MGMT_INVALID_INDEX;
}

/* Called with buf_mgmt.lock held from RX worker callback paths. Publish the
 * first overrun of an interval immediately through the board's callback-safe
 * event writer; sync_rx later carries the status bit without duplicating it. */
static void note_rx_overrun(struct bladerf_sync *s, uint32_t source_flags)
{
    struct buffer_mgmt *b = &s->buf_mgmt;
    b->overrun_pending = true;
    b->overrun_source_flags |= source_flags;
    if (!b->overrun_event_published && s->dev != NULL &&
        s->dev->board != NULL &&
        s->dev->board->rx_worker_stream_overrun != NULL) {
        b->overrun_event_published = true;
        s->dev->board->rx_worker_stream_overrun(
            s->dev, b->overrun_source_flags);
    }
    if (!b->stale_pending) {
        b->stale_pending = true;
        /* Wake the consumer once to discard stale data and resume at live
         * edge. Repeated overrun callbacks must not reset its timeout. */
        COND_SIGNAL(&b->buf_ready);
    }
}

bool sync_worker_rx_reorder_buffer(struct bladerf_sync *s, uint32_t seq,
                                   unsigned int buffer_idx,
                                   size_t num_samples)
{
    struct buffer_mgmt *b = &s->buf_mgmt;
    const uint32_t distance = seq - b->expected_seq;

    if (distance == 0) {
        return false;
    }

    if (distance <= b->reorder_limit &&
        hold_out_of_order_buffer(b, seq, buffer_idx, num_samples, false)) {
        return true;
    }

    if (distance > b->reorder_limit) {
        log_warning("RX reorder distance %u exceeds window %u; "
                    "forwarding buffer %u (expected %u)\n",
                    distance, b->reorder_limit, buffer_idx, b->expected_seq);
    } else {
        log_warning("RX reorder window full (limit=%u); forwarding buffer %u "
                    "(seq=%u, expected=%u)\n",
                    b->reorder_limit, buffer_idx, seq, b->expected_seq);
    }

    /* The caller may forward the payload, but consumers must be told that
     * worker ordering could not be preserved. META callers see the overrun
     * bit and validate timestamps; sample-only sync reads fail closed. */
    note_rx_overrun(s, BLADERF_RF_STREAM_STATUS_SYNC_RX_QUEUE |
                       BLADERF_RF_STREAM_STATUS_SYNC_RX_REORDER);
    return false;
}

void *sync_worker_rx_buffer_rejected(void *user_data, void *buffer)
{
    struct bladerf_sync *s = user_data;
    struct buffer_mgmt *b;
    unsigned int idx, next_idx;
    uint32_t seq;
    void *next_buffer;

    if (s == NULL || buffer == NULL ||
        (s->stream_config.layout & BLADERF_DIRECTION_MASK) != BLADERF_RX) {
        return buffer;
    }

    b = &s->buf_mgmt;
    if (b->buffer_seq == NULL) {
        return buffer;
    }

    MUTEX_LOCK(&b->lock);
    for (idx = 0; idx < b->num_buffers && b->buffers[idx] != buffer; ++idx) {
        /* Find the ring slot without depending on sync.c's address helper. */
    }
    if (idx >= b->num_buffers ||
        b->status[idx] != SYNC_BUFFER_IN_FLIGHT) {
        log_warning("Rejected RX buffer %p has no in-flight sync slot\n",
                    buffer);
        MUTEX_UNLOCK(&b->lock);
        return buffer;
    }

    seq = b->buffer_seq[idx];
    retire_dropped_sequence(s, seq);
    if (b->buffer_dropped != NULL) {
        b->buffer_dropped[idx] = true;
    }
    /* Wake only when the sync consumer is waiting on this exact slot. A
     * continuous epoch fence can reject many other transfers; waking on each
     * one would repeatedly restart sync_rx's wait and defeat its timeout. */
    if (idx == b->cons_i) {
        COND_SIGNAL(&b->buf_ready);
    }

    /* A rejected completion still consumed one submitted ring buffer. Rotate
     * to the same next slot that rx_callback() would have selected, so
     * producer indices and USB ownership cannot drift during an epoch fence. */
    b->status[idx] = SYNC_BUFFER_EMPTY;
    next_idx = sync_worker_rx_select_producer_slot(s);
    if (next_idx != BUFFER_MGMT_INVALID_INDEX) {
        sync_worker_mark_rx_slot_in_flight(
            b, next_idx, b->next_seq++);
        b->prod_i = (next_idx + 1) % b->num_buffers;
        next_buffer = b->buffers[next_idx];
    } else {
        /* This completion was rejected by the RX epoch admission fence, so
         * it contains no application-valid IQ to lose. If the consumer is
         * paused, the ring may be occupied by old-epoch entries; recycle the
         * same transport buffer and let the next sync_rx() discard/re-anchor
         * those entries. Classifying intentional epoch withholding as a
         * stream overrun would report a fault that did not affect valid data. */
        /* This slot now represents the newly submitted sequence. The
         * rejected old sequence was retired above; its marker must not make
         * sync_rx skip this live in-flight sequence. */
        sync_worker_mark_rx_slot_in_flight(b, idx, b->next_seq++);
        next_buffer = buffer;
    }
    MUTEX_UNLOCK(&b->lock);
    return next_buffer;
}

static void *rx_callback(struct bladerf *dev,
                         struct bladerf_stream *stream,
                         struct bladerf_metadata *meta,
                         void *samples,
                         size_t num_samples,
                         void *user_data)
{
    unsigned int requests;      /* Pending requests */
    unsigned int next_idx;
    unsigned int samples_idx;
    void *next_buf = NULL;      /* Next buffer to submit for reception */

    struct bladerf_sync *s = (struct bladerf_sync *)user_data;
    struct sync_worker  *w = s->worker;
    struct buffer_mgmt  *b = &s->buf_mgmt;

    /* Check if the caller has requested us to shut down. We'll keep the
     * SHUTDOWN bit set through our transition into the IDLE state so we
     * can act on it there. */
    MUTEX_LOCK(&w->request_lock);
    requests = w->requests;
    MUTEX_UNLOCK(&w->request_lock);

    if (requests & SYNC_WORKER_STOP) {
        log_verbose("%s worker: Got STOP request upon entering callback. "
                    "Ending stream.\n", worker2str(s));
        return NULL;
    }

    /* Event-only RX callbacks carry no buffer address. They signal an
     * invalidated/withheld transfer; never pass NULL to sync_buf2idx() or
     * mark an empty ring slot ready. Wake a blocked sync_rx() so it can return
     * WOULD_BLOCK, and recycle the actual USB buffer in async.c. */
    if (samples == NULL && num_samples == 0) {
        MUTEX_LOCK(&b->lock);
        b->rx_data_withheld_pending = true;
        COND_SIGNAL(&b->buf_ready);
        MUTEX_UNLOCK(&b->lock);
        return BLADERF_STREAM_REUSE_BUFFER;
    }

    MUTEX_LOCK(&b->lock);

    if (b->rx_epoch_trace_remaining != 0) {
        const unsigned int idx = sync_buf2idx(b, samples);
        log_debug("RX epoch worker callback: buffer=%u samples=%zu "
                  "prod_i=%u cons_i=%u slot_state=%u seq=%u expected_seq=%u\n",
                  idx, num_samples, b->prod_i, b->cons_i,
                  (unsigned)b->status[b->prod_i],
                  b->buffer_seq != NULL ? b->buffer_seq[idx] : 0,
                  b->expected_seq);
        (void)idx; /* log_debug arguments disappear in non-debug builds. */
        b->rx_epoch_trace_remaining--;
    }

    /* Get the index of the buffer that was just filled */
    samples_idx = sync_buf2idx(b, samples);
    if (b->buffer_dropped != NULL) {
        /* A later valid completion supersedes the empty-slot marker. */
        b->buffer_dropped[samples_idx] = false;
    }

    next_idx = sync_worker_rx_select_producer_slot(s);
    if (next_idx != BUFFER_MGMT_INVALID_INDEX) {

            bool release_now = true;
            uint32_t seq     = 0;

            if (b->buffer_seq) {
                seq = b->buffer_seq[samples_idx];

                if (seq != b->expected_seq) {
                    if (sync_worker_rx_reorder_buffer(
                            s, seq, samples_idx, num_samples)) {
                        release_now = false;
                    }
                }
            }

            if (release_now) {
                mark_buffer_ready(b, samples_idx, num_samples);

                if (b->buffer_seq && seq == b->expected_seq) {
                    b->expected_seq++;
                    flush_reorder_entries(b);
                }

                log_verbose("%s worker: buf[%u] = full\n",
                            worker2str(s), samples_idx);
            }

            /* Update the state of the buffer being submitted next */
            sync_worker_mark_rx_slot_in_flight(
                b, next_idx, b->next_seq++);
            next_buf = b->buffers[next_idx];

            /* Advance to the next buffer for the next callback */
            b->prod_i = (next_idx + 1) % b->num_buffers;

            if (release_now) {
                log_verbose("%s worker: buf[%u] now in_flight\n",
                            worker2str(s), next_idx);
            }

    } else {
            if (b->reorder_len > 0) {
                /* We're holding buffers to restore order. Skip submitting a
                 * new transfer this time to avoid overwriting held data. */
                next_buf = BLADERF_STREAM_NO_DATA;
                log_verbose("%s worker: delaying submission while reorder "
                            "queue drains\n", worker2str(s));
            } else {
                unsigned int full_slots = 0, partial_slots = 0;
                unsigned int in_flight_slots = 0, empty_slots = 0;
                unsigned int dropped_slots = 0;

                /* This branch is exceptional and already declares RX data
                 * loss. Keep the ring snapshot visible at WARNING level so
                 * production timing is not changed by per-transfer DEBUG
                 * logging when diagnosing an intermittent ring-full fault. */
                for (unsigned int i = 0; i < b->num_buffers; ++i) {
                    switch (b->status[i]) {
                        case SYNC_BUFFER_FULL: full_slots++; break;
                        case SYNC_BUFFER_PARTIAL: partial_slots++; break;
                        case SYNC_BUFFER_IN_FLIGHT: in_flight_slots++; break;
                        case SYNC_BUFFER_EMPTY: empty_slots++; break;
                        default: break;
                    }
                    if (b->buffer_dropped != NULL && b->buffer_dropped[i]) {
                        dropped_slots++;
                    }
                }
                log_warning("RX sync ring full: buffers=%u slot=%u "
                            "prod_i=%u prod_seq=%u prod_state=%u "
                            "completed_state=%u completed_seq=%u "
                            "states{full=%u partial=%u in_flight=%u empty=%u "
                            "dropped=%u} cursors{cons_i=%u partial_off=%u "
                            "expected_seq=%u next_seq=%u reorder_len=%u}\n",
                            b->num_buffers, samples_idx, b->prod_i,
                            b->buffer_seq != NULL
                                ? b->buffer_seq[b->prod_i] : 0,
                            (unsigned)b->status[b->prod_i],
                            (unsigned)b->status[samples_idx],
                            b->buffer_seq != NULL
                                ? b->buffer_seq[samples_idx] : 0,
                            full_slots, partial_slots, in_flight_slots,
                            empty_slots, dropped_slots, b->cons_i,
                            b->partial_off, b->expected_seq, b->next_seq,
                            b->reorder_len);

                /* This completed transfer is withheld. Retire its sequence
                 * and recycle the same transport buffer; stale FULL slots
                 * are removed by the RX_NOW consumer before admission resumes.
                 * The reissued slot must not retain a dropped marker from its
                 * just-retired sequence. */
                sync_worker_rx_ring_full_recycle(s, samples_idx);
                note_rx_overrun(s,
                    BLADERF_RF_STREAM_STATUS_SYNC_RX_QUEUE |
                    BLADERF_RF_STREAM_STATUS_SYNC_RX_RING_FULL);
                next_buf = samples;
            }
    }


    MUTEX_UNLOCK(&b->lock);
    return next_buf;
}

static void *tx_callback(struct bladerf *dev,
                         struct bladerf_stream *stream,
                         struct bladerf_metadata *meta,
                         void *samples,
                         size_t num_samples,
                         void *user_data)
{
    unsigned int requests;      /* Pending requests */
    unsigned int completed_idx; /* Index of completed buffer */

    struct bladerf_sync *s = (struct bladerf_sync *)user_data;
    struct sync_worker  *w = s->worker;
    struct buffer_mgmt  *b = &s->buf_mgmt;

    void *ret = BLADERF_STREAM_NO_DATA;

    /* Check if the caller has requested us to shut down. We'll keep the
     * SHUTDOWN bit set through our transition into the IDLE state so we
     * can act on it there. */
    MUTEX_LOCK(&w->request_lock);
    requests = w->requests;
    MUTEX_UNLOCK(&w->request_lock);

    if (requests & SYNC_WORKER_STOP) {
        log_verbose("%s worker: Got STOP request upon entering callback. "
                    "Ending stream.\r\n", worker2str(s));
        return NULL;
    }

    /* The initial set of callbacks will do not provide us with any
     * completed sample buffers */
    if (samples != NULL) {
        MUTEX_LOCK(&b->lock);

        /* Mark the completed buffer as being empty */
        completed_idx = sync_buf2idx(b, samples);
        assert(b->status[completed_idx] == SYNC_BUFFER_IN_FLIGHT);
        b->status[completed_idx] = SYNC_BUFFER_EMPTY;
        COND_SIGNAL(&b->buf_ready);

        /* If the callback is assigned to be the submitter, there are
         * buffers pending submission */
        if (b->submitter == SYNC_TX_SUBMITTER_CALLBACK) {
            assert(b->cons_i != BUFFER_MGMT_INVALID_INDEX);
            if (b->status[b->cons_i] == SYNC_BUFFER_FULL) {
                /* This buffer is ready to ship out ("consume") */
                log_verbose("%s: Submitting deferred buf[%u]\n",
                            __FUNCTION__, b->cons_i);

                ret = b->buffers[b->cons_i];
                /* This is actually # of 32bit DWORDs for PACKET_META */
                meta->actual_count = b->actual_lengths[b->cons_i];
                b->status[b->cons_i] = SYNC_BUFFER_IN_FLIGHT;
                b->cons_i = (b->cons_i + 1) % b->num_buffers;
            } else {
                log_verbose("%s: No deferred buffer available. "
                            "Assigning submitter=FN\n", __FUNCTION__);

                b->submitter = SYNC_TX_SUBMITTER_FN;
                b->cons_i = BUFFER_MGMT_INVALID_INDEX;
            }
        }

        MUTEX_UNLOCK(&b->lock);

        log_verbose("%s worker: Buffer %u emptied.\r\n",
                    worker2str(s), completed_idx);
    }

    return ret;
}

int sync_worker_init(struct bladerf_sync *s)
{
    int status = 0;
    bool stream_initialized = false;
    bool state_lock_initialized = false;
    bool request_lock_initialized = false;
    bool state_changed_initialized = false;
    bool requests_pending_initialized = false;

    s->worker  = (struct sync_worker *)calloc(1, sizeof(*s->worker));

    if (s->worker == NULL) {
        status = BLADERF_ERR_MEM;
        goto worker_init_out;
    }

    s->worker->state    = SYNC_WORKER_STATE_STARTUP;
    s->worker->err_code = 0;

    s->worker->cb =
        (s->stream_config.layout & BLADERF_DIRECTION_MASK) == BLADERF_RX
            ? rx_callback
            : tx_callback;

    status = async_init_stream(
        &s->worker->stream, s->dev, s->worker->cb, &s->buf_mgmt.buffers,
        s->buf_mgmt.num_buffers, s->stream_config.format,
        s->stream_config.samples_per_buffer, s->stream_config.num_xfers, s);

    if (status != 0) {
        log_debug("%s worker: Failed to init stream: %s\n", worker2str(s),
                  bladerf_strerror(status));
        goto worker_init_out;
    }
    stream_initialized = true;

    status = async_set_transfer_timeout(
        s->worker->stream,
        sync_worker_transfer_timeout_ms(
            s->stream_config.layout & BLADERF_DIRECTION_MASK,
            s->stream_config.format, s->stream_config.timeout_ms));
    if (status != 0) {
        log_debug("%s worker: Failed to set transfer timeout: %s\n",
                  worker2str(s), bladerf_strerror(status));
        goto worker_init_out;
    }

    if ((s->stream_config.layout & BLADERF_DIRECTION_MASK) == BLADERF_RX) {
        s->worker->stream->rx_buffer_rejected =
            sync_worker_rx_buffer_rejected;
    }

    MUTEX_INIT(&s->worker->state_lock);
    state_lock_initialized = true;
    MUTEX_INIT(&s->worker->request_lock);
    request_lock_initialized = true;

    status = COND_INIT(&s->worker->state_changed);
    if (status != THREAD_SUCCESS) {
        log_debug("%s worker: cond_init(state_changed) failed: %d\n",
                  worker2str(s), status);
        status = BLADERF_ERR_UNEXPECTED;
        goto worker_init_out;
    }
    state_changed_initialized = true;

    status = COND_INIT(&s->worker->requests_pending);
    if (status != THREAD_SUCCESS) {
        log_debug("%s worker: cond_init(requests_pending) failed: %d\n",
                  worker2str(s), status);
        status = BLADERF_ERR_UNEXPECTED;
        goto worker_init_out;
    }
    requests_pending_initialized = true;

    status = THREAD_CREATE(&s->worker->thread, sync_worker_task, s);
    if (status != THREAD_SUCCESS) {
        log_debug("%s worker: create failed: %d\n", worker2str(s),
                  status);
        status = BLADERF_ERR_UNEXPECTED;
        goto worker_init_out;
    }

    /* Wait until the worker thread has initialized and is ready to go */
    status =
        sync_worker_wait_for_state(s->worker, SYNC_WORKER_STATE_IDLE, 10000);
    if (status != 0) {
        log_debug("%s worker: sync_worker_wait_for_state failed: %d\n",
                  worker2str(s), status);
        status = BLADERF_ERR_TIMEOUT;
        /* The thread exists even if it missed the startup deadline. Stop and
         * join it while its state locks and storage are still alive; freeing
         * s->worker here races the worker's initial set_state(). */
        sync_worker_request_stop_and_join(s->worker, &s->buf_mgmt.lock,
                                          &s->buf_mgmt.buf_ready);
        goto worker_init_out;
    }

worker_init_out:
    if (status != 0) {
        if (stream_initialized) {
            async_deinit_stream(s->worker->stream);
        }
        if (requests_pending_initialized) {
            COND_DESTROY(&s->worker->requests_pending);
        }
        if (state_changed_initialized) {
            COND_DESTROY(&s->worker->state_changed);
        }
        if (request_lock_initialized) {
            MUTEX_DESTROY(&s->worker->request_lock);
        }
        if (state_lock_initialized) {
            MUTEX_DESTROY(&s->worker->state_lock);
        }
        free(s->worker);
        s->worker = NULL;
    }

    return status;
}

void sync_worker_request_stop_and_join(struct sync_worker *w,
                                      MUTEX *lock, COND *cond)
{
    int status;

    if (w == NULL) {
        log_debug("%s called with NULL ptr\n", __FUNCTION__);
        return;
    }

    log_verbose("%s: Requesting worker %p to stop...\n", __FUNCTION__, w);
    log_debug("sync_worker_deinit: request stop worker=%p\n", (void *)w);

    sync_worker_submit_request(w, SYNC_WORKER_STOP);

    if (lock != NULL && cond != NULL) {
        MUTEX_LOCK(lock);
        COND_SIGNAL(cond);
        MUTEX_UNLOCK(lock);
    }

    status = sync_worker_wait_for_state(w, SYNC_WORKER_STATE_STOPPED, 3000);
    log_debug("sync_worker_deinit: stopped wait worker=%p status=%d\n",
              (void *)w, status);

    if (status != 0) {
        log_warning("Timed out while stopping worker. Canceling thread.\n");
        THREAD_CANCEL(w->thread);
    }

    THREAD_JOIN(w->thread, NULL);
    log_verbose("%s: Worker joined.\n", __FUNCTION__);
}

void sync_worker_deinit(struct sync_worker *w,
                        MUTEX *lock, COND *cond)
{
    if (w == NULL) {
        log_debug("%s called with NULL ptr\n", __FUNCTION__);
        return;
    }

    sync_worker_request_stop_and_join(w, lock, cond);

    log_debug("sync_worker_deinit: async stream deinit begin worker=%p\n",
              (void *)w);
    async_deinit_stream(w->stream);
    log_debug("sync_worker_deinit: async stream deinit complete worker=%p\n",
              (void *)w);

    COND_DESTROY(&w->requests_pending);
    COND_DESTROY(&w->state_changed);
    MUTEX_DESTROY(&w->request_lock);
    MUTEX_DESTROY(&w->state_lock);

    free(w);
}

void sync_worker_submit_request(struct sync_worker *w, unsigned int request)
{
    MUTEX_LOCK(&w->request_lock);
    w->requests |= request;
    COND_SIGNAL(&w->requests_pending);
    MUTEX_UNLOCK(&w->request_lock);

    /* A stalled RX or TX worker may be waiting for its next transfer callback
     * before it can observe STOP. Request stream shutdown directly so the USB
     * backend cancels and drains outstanding transfers, then wakes the worker
     * through its ordinary callback/STREAM_DONE path. Without this nudge the
     * worker can be cancelled while the backend event thread waits forever
     * for STREAM_DONE. */
    if ((request & SYNC_WORKER_STOP) && w->stream != NULL) {
        MUTEX_LOCK(&w->stream->lock);
        if (w->stream->state == STREAM_RUNNING) {
            w->stream->state = STREAM_SHUTTING_DOWN;
        }
        MUTEX_UNLOCK(&w->stream->lock);
    }
}

int sync_worker_wait_for_state(struct sync_worker *w, sync_worker_state state,
                               unsigned int timeout_ms)
{
    int status = 0;

    if (timeout_ms != 0) {
        MUTEX_LOCK(&w->state_lock);
        status = 0;
        while (w->state != state && status == THREAD_SUCCESS) {
            status = COND_TIMED_WAIT(&w->state_changed,
                                            &w->state_lock,
                                            timeout_ms);
        }
        MUTEX_UNLOCK(&w->state_lock);

    } else {
        MUTEX_LOCK(&w->state_lock);
        while (w->state != state) {
            log_verbose(": Waiting for state change, current = %d\n", w->state);
            status = COND_WAIT(&w->state_changed,
                                       &w->state_lock);
        }
        MUTEX_UNLOCK(&w->state_lock);
    }

    if (status != 0) {
        log_debug("%s: Wait on state change failed: %s\n",
                   __FUNCTION__, strerror(status));

        if (status == THREAD_TIMEOUT) {
            status = BLADERF_ERR_TIMEOUT;
        } else {
            status = BLADERF_ERR_UNEXPECTED;
        }
    }

    return status;
}

sync_worker_state sync_worker_get_state(struct sync_worker *w,
                                        int *err_code)
{
    sync_worker_state ret;

    MUTEX_LOCK(&w->state_lock);
    ret = w->state;
    if (err_code) {
        *err_code = w->err_code;
        w->err_code = 0;
    }
    MUTEX_UNLOCK(&w->state_lock);

    return ret;
}

static void set_state(struct sync_worker *w, sync_worker_state state)
{
    MUTEX_LOCK(&w->state_lock);
    w->state = state;
    COND_SIGNAL(&w->state_changed);
    MUTEX_UNLOCK(&w->state_lock);
}


static sync_worker_state exec_idle_state(struct bladerf_sync *s)
{
    sync_worker_state next_state = SYNC_WORKER_STATE_IDLE;
    unsigned int requests;
    unsigned int i;

    MUTEX_LOCK(&s->worker->request_lock);

    while (s->worker->requests == 0) {
        log_verbose("%s worker: Waiting for pending requests\n", worker2str(s));

        COND_WAIT(&s->worker->requests_pending,
                          &s->worker->request_lock);
    }

    requests = s->worker->requests;
    s->worker->requests = 0;
    MUTEX_UNLOCK(&s->worker->request_lock);

    if (requests & SYNC_WORKER_STOP) {
        log_verbose("%s worker: Got request to stop\n", worker2str(s));

        next_state = SYNC_WORKER_STATE_SHUTTING_DOWN;

    } else if (requests & SYNC_WORKER_START) {
        log_verbose("%s worker: Got request to start\n", worker2str(s));
        MUTEX_LOCK(&s->buf_mgmt.lock);

        if ((s->stream_config.layout & BLADERF_DIRECTION_MASK) == BLADERF_TX) {
            /* If we've previously timed out on a stream, we'll likely have some
            * stale buffers marked "in-flight" that have since been cancelled. */
            for (i = 0; i < s->buf_mgmt.num_buffers; i++) {
                if (s->buf_mgmt.status[i] == SYNC_BUFFER_IN_FLIGHT) {
                    s->buf_mgmt.status[i] = SYNC_BUFFER_EMPTY;
                }
            }

            COND_SIGNAL(&s->buf_mgmt.buf_ready);
        } else {
            s->buf_mgmt.prod_i = s->stream_config.num_xfers;

            for (i = 0; i < s->buf_mgmt.num_buffers; i++) {
                if (i < s->stream_config.num_xfers) {
                    s->buf_mgmt.status[i] = SYNC_BUFFER_IN_FLIGHT;
                } else if (s->buf_mgmt.status[i] == SYNC_BUFFER_IN_FLIGHT) {
                    s->buf_mgmt.status[i] = SYNC_BUFFER_EMPTY;
                }
            }

            sync_reset_sequence_tracking(&s->buf_mgmt,
                                         s->stream_config.num_xfers);
        }

        MUTEX_UNLOCK(&s->buf_mgmt.lock);

        next_state = SYNC_WORKER_STATE_RUNNING;
    } else {
        log_warning("Invalid request value encountered: 0x%08X\n",
                    s->worker->requests);
    }

    return next_state;
}

static void exec_running_state(struct bladerf_sync *s)
{
    int status;

    status = async_run_stream(s->worker->stream, s->stream_config.layout);

    log_verbose("%s worker: stream ended with: %s\n",
                worker2str(s), bladerf_strerror(status));

    /* Save off the result of running the stream so we can report what
     * happened to the API caller */
    MUTEX_LOCK(&s->worker->state_lock);
    s->worker->err_code = status;
    MUTEX_UNLOCK(&s->worker->state_lock);

    /* Wake the API-side if an error occurred, so that it can propagate
     * the stream error code back to the API caller */
    if (status != 0) {
        MUTEX_LOCK(&s->buf_mgmt.lock);
        COND_SIGNAL(&s->buf_mgmt.buf_ready);
        MUTEX_UNLOCK(&s->buf_mgmt.lock);
    }
}

void *sync_worker_task(void *arg)
{
    sync_worker_state state = SYNC_WORKER_STATE_IDLE;
    struct bladerf_sync *s = (struct bladerf_sync *)arg;

    log_verbose("%s worker: task started\n", worker2str(s));
    set_state(s->worker, state);
    log_verbose("%s worker: task state set\n", worker2str(s));

    while (state != SYNC_WORKER_STATE_STOPPED) {

        switch (state) {
            case SYNC_WORKER_STATE_STARTUP:
                assert(!"Worker in unexpected state, shutting down. (STARTUP)");
                set_state(s->worker, SYNC_WORKER_STATE_SHUTTING_DOWN);
                break;

            case SYNC_WORKER_STATE_IDLE:
                state = exec_idle_state(s);
                set_state(s->worker, state);
                break;

            case SYNC_WORKER_STATE_RUNNING:
                exec_running_state(s);
                state = SYNC_WORKER_STATE_IDLE;
                set_state(s->worker, state);
                break;

            case SYNC_WORKER_STATE_SHUTTING_DOWN:
                log_verbose("%s worker: Shutting down...\n", worker2str(s));

                state = SYNC_WORKER_STATE_STOPPED;
                set_state(s->worker, state);
                break;

            case SYNC_WORKER_STATE_STOPPED:
                assert(!"Worker in unexpected state: STOPPED");
                break;

            default:
                assert(!"Worker in unexpected state, shutting down. (UNKNOWN)");
                set_state(s->worker, SYNC_WORKER_STATE_SHUTTING_DOWN);
                break;
        }
    }

    return NULL;
}
