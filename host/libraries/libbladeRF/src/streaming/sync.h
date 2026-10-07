/*
 * This file is part of the bladeRF project:
 *   http://www.github.com/nuand/bladeRF
 *
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef STREAMING_SYNC_H_
#define STREAMING_SYNC_H_

#include <limits.h>
#include <stdint.h>

#include <libbladeRF.h>

#include "thread.h"

/* These parameters are only written during sync_init */
struct stream_config {
    bladerf_format format;
    bladerf_channel_layout layout;

    unsigned int samples_per_buffer;
    unsigned int num_xfers;
    unsigned int timeout_ms;

    size_t bytes_per_sample;
};

typedef enum {
    SYNC_BUFFER_EMPTY = 0, /**< Buffer contains no data */
    SYNC_BUFFER_PARTIAL,   /**< sync_rx/tx is currently emptying/filling */
    SYNC_BUFFER_FULL,      /**< Buffer is full of data */
    SYNC_BUFFER_IN_FLIGHT, /**< Currently being transferred */
} sync_buffer_status;

#ifndef SYNC_RX_MAX_REORDER
#define SYNC_RX_MAX_REORDER 256
#endif

struct rx_reorder_entry {
    uint32_t seq;
    unsigned int buf_idx;
    size_t num_samples;
    bool dropped;
};

typedef enum {
    SYNC_META_STATE_HEADER,  /**< Extract the metadata header */
    SYNC_META_STATE_SAMPLES, /**< Process samples */
} sync_meta_state;

typedef enum {
    /** Invalid selection */
    SYNC_TX_SUBMITTER_INVALID = -1,

    /** sync_tx() is repsonsible for submitting buffers for async transfer */
    SYNC_TX_SUBMITTER_FN,

    /** The TX worker callbacks should be returning buffers for submission  */
    SYNC_TX_SUBMITTER_CALLBACK
} sync_tx_submitter;

#define BUFFER_MGMT_INVALID_INDEX (UINT_MAX)

struct buffer_mgmt {
    sync_buffer_status *status;
    size_t *actual_lengths;

    void **buffers;
    unsigned int num_buffers;

    unsigned int prod_i;      /**< Producer index - next buffer to fill */
    unsigned int cons_i;      /**< Consumer index - next buffer to empty */
    unsigned int partial_off; /**< Current index into partial buffer */

    /* Set by the RX worker when it detects an overrun, cleared once the
     * condition has been reported to a bladerf_sync_rx() caller. The worker
     * recovers by resubmitting buffers, so the sample stream has a gap that
     * is otherwise invisible to the caller. */
    bool overrun_pending;
    uint32_t overrun_source_flags;
    /* The sync worker publishes the first overrun in an interval directly to
     * the device event history; sync_rx clears this latch with the status. */
    bool overrun_event_published;

    /* Async epoch gating can wake the sync worker without a sample buffer.
     * The next sync_rx() must fail closed instead of treating that callback
     * as a completed ring buffer. */
    bool rx_data_withheld_pending;

    /* Also set by the RX worker on an overrun, cleared once the consumer
     * has dropped the buffers that were already full at that moment. Those
     * buffers hold the OLDEST samples: everything the hardware produced
     * after the ring filled was discarded, so on resume the consumer would
     * read history first - up to (num_buffers - num_transfers) buffers of
     * it. With a metadata format the timestamps expose this; without one
     * the stale prefix is indistinguishable from live data. */
    bool stale_pending;

    /* Applicable to TX only. Denotes which context is responsible for
     * submitting full buffers to the underlying async system */
    sync_tx_submitter submitter;

    /* RX-specific tracking for out-of-order completions */
    uint32_t *buffer_seq;
    /* Rejected RX transfers still own their USB buffers, but the sync
     * consumer must be able to skip the corresponding empty ring slots. */
    bool *buffer_dropped;
    uint32_t expected_seq;
    uint32_t next_seq;
    struct rx_reorder_entry reorder[SYNC_RX_MAX_REORDER];
    size_t reorder_len;
    unsigned int reorder_limit;

    /* At DEBUG verbosity, trace only a few RX callbacks after each boundary
     * to localize worker handoff problems without hot-path log floods. */
    unsigned int rx_epoch_trace_remaining;


    MUTEX lock;
    COND buf_ready;           /**< Buffer produced by RX callback, or
                               *   buffer emptied by TX callback */
};

static inline void sync_reset_sequence_tracking(struct buffer_mgmt *b,
                                                unsigned int num_in_flight)
{
    if (b == NULL) {
        return;
    }

    if (!b->buffer_seq) {
        b->expected_seq = 0;
        b->next_seq     = 0;
        b->reorder_len  = 0;
        return;
    }

    for (size_t i = 0; i < SYNC_RX_MAX_REORDER; ++i) {
        b->reorder[i].seq        = 0;
        b->reorder[i].buf_idx    = 0;
        b->reorder[i].num_samples = 0;
    }

    for (unsigned int i = 0; i < b->num_buffers; ++i) {
        b->buffer_seq[i] = UINT32_MAX;
    }

    unsigned int limit = num_in_flight;
    if (limit > b->num_buffers) {
        limit = b->num_buffers;
    }

    for (unsigned int i = 0; i < limit; ++i) {
        b->buffer_seq[i] = i;
    }

    b->expected_seq = 0;
    b->next_seq     = limit;
    b->reorder_len  = 0;
    /* Allow a larger reorder window independent of instantaneous slack. */
    if (b->num_buffers > 1) {
        unsigned int capacity = b->num_buffers - 1;
        if (capacity > SYNC_RX_MAX_REORDER) {
            capacity = SYNC_RX_MAX_REORDER;
        }
        b->reorder_limit = capacity;
    } else {
        b->reorder_limit = 0;
    }
}

/* On RX_NOW, stale buffers are older than the requested live edge and may be
 * dropped after an overrun. Timestamp-targeted META reads must retain them. */
static inline bool sync_rx_should_drop_stale(bladerf_format format,
                                             uint32_t metadata_flags)
{
    if (format == BLADERF_FORMAT_PACKET_META) {
        return false;
    }
    if (format == BLADERF_FORMAT_SC16_Q11_META ||
        format == BLADERF_FORMAT_SC8_Q7_META) {
        return (metadata_flags & BLADERF_META_FLAG_RX_NOW) != 0;
    }
    return true;
}

/* State of API-side sync interface */
typedef enum {
    SYNC_STATE_CHECK_WORKER,
    SYNC_STATE_RESET_BUF_MGMT,
    SYNC_STATE_START_WORKER,
    SYNC_STATE_WAIT_FOR_BUFFER,
    SYNC_STATE_BUFFER_READY,
    SYNC_STATE_USING_BUFFER,
    SYNC_STATE_USING_PACKET_META,
    SYNC_STATE_USING_BUFFER_META
} sync_state;

struct sync_meta {
    sync_meta_state state; /* State of metadata processing */

    bool have_timestamp;   /* curr_timestamp holds a value read from a
                            * message header. Until then there is nothing to
                            * compare against, so the first header of a
                            * stream must not be treated as a discontinuity. */

    uint8_t *curr_msg;            /* Points to current message in the buffer */
    size_t curr_msg_off;          /* Offset into current message (samples),
                                   * ignoring the 4-samples worth of metadata */
    size_t msg_size;              /* Size of data message */
    unsigned int msg_per_buf;     /* Number of data messages per buffer */
    unsigned int msg_num;         /* Which message within the buffer are we in?
                                   * Range is: 0 to msg_per_buf   */
    unsigned int samples_per_msg; /* Number of samples within a message */
    unsigned int samples_per_ts;  /* Number of samples within a timestamp */

    union {
        /* Used only for RX */
        struct {
            uint64_t
                msg_timestamp;  /* Timestamp contained in the current message */
            uint32_t msg_flags; /* Flags for the current message */
            uint8_t msg_epoch_id;
            bool msg_epoch_id_valid;
            bool msg_epoch_filtered_out;
            bool msg_channel_filtered_out;
        };

        /* Used only for TX */
        struct {
            bool in_burst;
            uint64_t burst_start; /* Timestamp the current burst was
                                   * scheduled to begin at. Valid while
                                   * in_burst && !now; used to bound how
                                   * much of the burst may sit in the
                                   * device ahead of playback. */
            bool now;
        };
    };

    uint64_t curr_timestamp; /* Timestamp at the sample we've
                              * consumed up to */
    bool rx_epoch_boundary_enabled;
    uint64_t rx_epoch_min_timestamp;
    bool rx_epoch_id_filter_enabled;
    bool rx_epoch_data_invalidated;
    uint8_t rx_epoch_expected_id;
};

struct bladerf_sync {
    MUTEX lock;
    /* Epoch invalidation must cancel a blocked sync_rx before acquiring
     * lock: rejected old-epoch callbacks can otherwise wake that reader
     * indefinitely while a transition waits for the same lock. */
    MUTEX rx_epoch_generation_lock;
    uint64_t rx_epoch_generation;
    struct bladerf *dev;
    bool initialized;
    sync_state state;
    struct buffer_mgmt buf_mgmt;
    struct stream_config stream_config;
    struct sync_worker *worker;
    struct sync_meta meta;
};

/**
 * Create and initialize as synchronous interface handle for the specified
 * device and direction. If the synchronous handle is already initialized, this
 * call will first deinitialize it.
 *
 * The associated stream will be started at the first RX or TX call
 *
 * @return 0 or BLADERF_ERR_* value on failure
 */
int sync_init(struct bladerf_sync *sync,
              struct bladerf *dev,
              bladerf_channel_layout layout,
              bladerf_format format,
              unsigned int num_buffers,
              size_t buffer_size,
              size_t msg_size,
              unsigned int num_transfers,
              unsigned int stream_timeout);

/**
 * Deinitialize the sync handle. This tears down and deallocates the underlying
 * asynchronous stream.
 *
 * @param[inout]    sync    Handle to deinitialize.
 */
void sync_deinit(struct bladerf_sync *sync);

int sync_rx(struct bladerf_sync *sync,
            void *samples,
            unsigned int num_samples,
            struct bladerf_metadata *metadata,
            unsigned int timeout_ms);

int sync_tx(struct bladerf_sync *sync,
            void const *samples,
            unsigned int num_samples,
            struct bladerf_metadata *metadata,
            unsigned int timeout_ms);

unsigned int sync_buf2idx(struct buffer_mgmt *b, void *addr);

void *sync_idx2buf(struct buffer_mgmt *b, unsigned int idx);

int sync_prime_stream(struct bladerf_sync *sync, unsigned int timeout_ms);

/* RX transition integration. Raw sample formats cannot distinguish queued
 * old IQ; epoch fencing therefore requires per-message RX timestamps. */
int sync_rx_epoch_require_metadata(struct bladerf_sync *sync);
int sync_rx_epoch_invalidate(struct bladerf_sync *sync);
void sync_rx_report_fpga_loss(struct bladerf_sync *sync);
bool sync_rx_epoch_filter_enabled(struct bladerf_sync *sync);
int sync_rx_epoch_require_transition(struct bladerf_sync *sync);
int sync_rx_epoch_expect_id(struct bladerf_sync *sync, uint8_t epoch_id);
int sync_rx_epoch_set_min_timestamp(struct bladerf_sync *sync,
                                    uint64_t min_timestamp,
                                    uint8_t epoch_id);
/* Apply the final RX timestamp fence only before the transition deadline;
 * an expired call leaves RX epoch data invalidated. */
int sync_rx_epoch_set_min_timestamp_before_deadline(
    struct bladerf_sync *sync, uint64_t min_timestamp, uint8_t epoch_id,
    uint64_t deadline_ns);
int sync_rx_epoch_stage_min_timestamp_before_deadline(
    struct bladerf_sync *sync, uint64_t min_timestamp, uint8_t epoch_id,
    uint64_t deadline_ns);
int sync_rx_epoch_activate_before_deadline(struct bladerf_sync *sync,
                                           uint64_t deadline_ns);
typedef int (*sync_rx_epoch_admission_prepare_fn)(void *context,
                                                  uint64_t deadline_ns);
typedef void (*sync_rx_epoch_admission_finish_fn)(void *context);
int sync_rx_epoch_activate_with_admission_before_deadline(
    struct bladerf_sync *sync, uint64_t deadline_ns,
    sync_rx_epoch_admission_prepare_fn prepare,
    sync_rx_epoch_admission_finish_fn finish, void *context);

#endif
