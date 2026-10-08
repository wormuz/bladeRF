/*
 * Copyright (C) 2014 Nuand LLC
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

#ifndef STREAMING_SYNC_WORKER_H_
#define STREAMING_SYNC_WORKER_H_

#include "host_config.h"
#include "sync.h"
#include <libbladeRF.h>
#include "thread.h"

#if BLADERF_OS_WINDOWS || BLADERF_OS_OSX
#include "clock_gettime.h"
#else
#include <time.h>
#endif

/* Worker lifetime:
 *
 * STARTUP --+--> IDLE --> RUNNING --+--> SHUTTING_DOWN --> STOPPED
 *           ^----------------------/
 */

/* Request flags */
#define SYNC_WORKER_START (1 << 0)
#define SYNC_WORKER_STOP (1 << 1)

typedef enum {
    SYNC_WORKER_STATE_STARTUP,
    SYNC_WORKER_STATE_IDLE,
    SYNC_WORKER_STATE_RUNNING,
    SYNC_WORKER_STATE_SHUTTING_DOWN,
    SYNC_WORKER_STATE_STOPPED
} sync_worker_state;

struct sync_worker {
    THREAD thread;

    struct bladerf_stream *stream;
    bladerf_stream_cb cb;

    /* These items should be accessed while holding state_lock */
    sync_worker_state state;
    int err_code;
    MUTEX state_lock;
    COND state_changed;           /* Worker thread uses this to inform a
                                   * waiting main thread about a state
                                   * change */

    /* The requests lock should always be acquired AFTER
     * the sync->buf_mgmt.lock
     */
    unsigned int requests;
    COND requests_pending;
    MUTEX request_lock;
};

/**
 * Create a launch a worker thread. It will enter the IDLE state upon
 * executing.
 *
 * @param   s   Sync handle containing worker to initialize
 *
 * @return 0 on success, BLADERF_ERR_* on failure
 */
int sync_worker_init(struct bladerf_sync *s);

/**
 * Shutdown and deinitialize
 *
 * @param       w       Worker to deinitialize
 * @param[in]   lock    Acquired to signal `cond` if non-NULL
 * @param[in]   cond    If non-NULL, this is signaled after requesting the
 *                      worker to shut down, waking a potentially blocked
 *                      workers.
 */
void sync_worker_deinit(struct sync_worker *w,
                        MUTEX *lock,
                        COND *cond);

/**
 * Wait for state change with optional timeout
 *
 * @param       w           Worker to wait for
 * @param[in]   state       State to wait for
 * @param[in]   timeout_ms  Timeout in ms. 0 implies "wait forever"
 *
 * @return 0 on success, BLADERF_ERR_TIMEOUT on timeout, BLADERF_ERR_UNKNOWN on
 * other errors
 */
int sync_worker_wait_for_state(struct sync_worker *w,
                               sync_worker_state state,
                               unsigned int timeout_ms);

/**
 * Get the worker's current state.
 *
 * @param       w           Worker to query
 * @param[out]  err_code    Stream error code (libbladeRF error code value).
 *                          Querying this value will reset the interal error
 *                          code value.
 *
 * @return Worker's current state
 */
sync_worker_state sync_worker_get_state(struct sync_worker *w, int *err_code);

/**
 * Submit a request to the worker task
 *
 * @param       w           Worker to send request to
 * @param[in]   request     Bitmask of requests to submit
 */
void sync_worker_submit_request(struct sync_worker *w, unsigned int request);

/* async.c calls this for each RX transfer withheld before rx_callback(). */
void *sync_worker_rx_buffer_rejected(void *user_data, void *buffer);

/* Discard completed RX ring entries that precede a newly fenced epoch.
 * Called with the sync lock held; this function acquires buf_mgmt.lock. */
static inline void sync_worker_discard_rx_epoch(struct bladerf_sync *s)
{
    struct buffer_mgmt *b;

    if (s == NULL || !s->initialized ||
        (s->stream_config.layout & BLADERF_DIRECTION_MASK) != BLADERF_RX) {
        return;
    }

    b = &s->buf_mgmt;
    MUTEX_LOCK(&b->lock);
    for (unsigned int i = 0; i < b->num_buffers; ++i) {
        if (b->status[i] == SYNC_BUFFER_FULL ||
            b->status[i] == SYNC_BUFFER_PARTIAL) {
            /* These samples were certified under the epoch being revoked.
             * Free their slots immediately and let WAIT_FOR_BUFFER skip
             * them without classifying the intentional fence as transport
             * loss. A later completion clears this marker before reuse. */
            b->status[i] = SYNC_BUFFER_EMPTY;
            b->actual_lengths[i] = 0;
            if (i == b->cons_i) {
                b->partial_off = 0;
            }
            if (b->buffer_dropped != NULL) {
                b->buffer_dropped[i] = true;
            }
        }
    }
    MUTEX_UNLOCK(&b->lock);
}

/* Epoch-fenced packets can visit every ring slot while the application is
 * paused. If every dropped marker is skipped, cons_i wraps to its old value,
 * which may now refer to a much later in-flight sequence. Re-anchor the
 * consumer to the slot carrying expected_seq so it waits for the oldest live
 * completion rather than blocking behind a stale ring index. Caller holds
 * buf_mgmt.lock. */
static inline bool sync_worker_reanchor_rx_consumer_to_expected(
    struct bladerf_sync *s)
{
    struct buffer_mgmt *b;

    if (s == NULL || !s->initialized) {
        return false;
    }

    b = &s->buf_mgmt;
    if (b->status == NULL || b->buffer_seq == NULL || b->num_buffers == 0) {
        return false;
    }

    for (unsigned int i = 0; i < b->num_buffers; ++i) {
        if (b->status[i] != SYNC_BUFFER_EMPTY &&
            b->buffer_seq[i] == b->expected_seq) {
            b->cons_i = i;
            b->partial_off = 0;
            return true;
        }
    }

    return false;
}

/* A rejected-marker run can end just short of a full ring when an EMPTY slot
 * retains a sequence from an older epoch. If that slot is not marked dropped,
 * the consumer otherwise waits there forever even though the producer has
 * advanced expected_seq and the matching live transfer occupies another
 * slot. Preserve a FULL/expected head; re-anchor only from a stale EMPTY slot
 * or after a complete ring of rejected markers. Caller holds buf_mgmt.lock. */
static inline bool sync_worker_reanchor_rx_consumer_after_rejections(
    struct bladerf_sync *s, unsigned int skipped)
{
    struct buffer_mgmt *b;
    bool stale_empty;

    if (s == NULL || !s->initialized) {
        return false;
    }
    b = &s->buf_mgmt;
    if (b->status == NULL || b->buffer_seq == NULL || b->num_buffers == 0 ||
        b->cons_i >= b->num_buffers) {
        return false;
    }

    stale_empty = b->status[b->cons_i] == SYNC_BUFFER_EMPTY &&
                  b->buffer_seq[b->cons_i] != b->expected_seq;
    if (skipped != b->num_buffers && !stale_empty) {
        return false;
    }

    return sync_worker_reanchor_rx_consumer_to_expected(s);
}

/* Hold an out-of-order RX buffer for later ordered delivery when it fits the
 * reorder window. Otherwise mark a discontinuity and wake sync_rx. The caller
 * must hold sync->buf_mgmt.lock. Returns true when the buffer was held. */
bool sync_worker_rx_reorder_buffer(struct bladerf_sync *sync, uint32_t seq,
                                   unsigned int buffer_idx,
                                   size_t num_samples);

#endif
