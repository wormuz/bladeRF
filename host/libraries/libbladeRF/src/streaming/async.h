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

#ifndef STREAMING_ASYNC_H_
#define STREAMING_ASYNC_H_

#include <libbladeRF.h>

#include "thread.h"

#include "format.h"

typedef enum {
    STREAM_IDLE,          /* Idle and initialized */
    STREAM_RUNNING,       /* Currently running */
    STREAM_SHUTTING_DOWN, /* Currently tearing down.
                           * See bladerf_stream->error_code to determine
                           * whether or not the shutdown was a clean exit
                           * or due to an error. */
    STREAM_DONE           /* Done and deallocated */
} bladerf_stream_state;

struct bladerf_stream {
    /* These items are configured in async_init_stream() and should only be
     * read (NOT MODIFIED) during the execution of the stream */
    struct bladerf *dev;
    bladerf_channel_layout layout;
    bladerf_format format;
    unsigned int transfer_timeout;
    bladerf_stream_cb cb;
    void *user_data;
    /* Optional sync-RX bookkeeping hook. Called for each transfer rejected
     * before the sample callback so sequence tracking can retire its slot. */
    void *(*rx_buffer_rejected)(void *user_data, void *buffer);
    size_t samples_per_buffer;
    size_t num_buffers;
    void **buffers;

    MUTEX lock;

    /* The following items must be accessed atomically */
    int error_code;
    bladerf_stream_state state;
    /* Protected by lock; coalesces event-only callbacks while continuous RX
     * data is withheld. */
    bool rx_withheld_notice_active;
    COND can_submit_buffer;
    COND stream_started;
    void *backend_data;
};

/* Get the number of bytes per stream buffer */
static inline size_t async_stream_buf_bytes(struct bladerf_stream *s)
{
    return samples_to_bytes(s->format, s->samples_per_buffer);
}

int async_init_stream(struct bladerf_stream **stream,
                      struct bladerf *dev,
                      bladerf_stream_cb callback,
                      void ***buffers,
                      size_t num_buffers,
                      bladerf_format format,
                      size_t buffer_size,
                      size_t num_transfers,
                      void *user_data);

/* Publish a receive discontinuity detected by an asynchronous backend. */
void async_notify_rx_overrun(struct bladerf_stream *stream);
/* Called with stream->lock held. Report a terminal RX transport failure to
 * native history and to the stream callback as an event-only notification.
 * Application callback execution temporarily releases that lock. No IQ is
 * delivered. */
void async_notify_rx_transport_failure(struct bladerf_stream *stream,
                                       uint32_t reason);

/* Called by RX backends with stream->lock held. Deliver a complete
 * continuous-IQ RX transfer, or recycle a partial one without exposing its
 * samples. The function drops stream->lock while invoking stream->cb so
 * callback code does not re-enter the backend while holding its state mutex,
 * then reacquires it before returning. Packet-meta payloads are caller-framed
 * and may be variable length, so they are passed through unchanged. */
void *async_rx_process_buffer(struct bladerf_stream *stream,
                              struct bladerf_metadata *metadata,
                              void *samples,
                              size_t received_bytes);

/* Set the transfer timeout. This acquires stream->lock. */
int async_set_transfer_timeout(struct bladerf_stream *stream,
                               unsigned int transfer_timeout_ms);

/* Get the transfer timeout. This acquires stream->lock. */
int async_get_transfer_timeout(struct bladerf_stream *stream,
                               unsigned int *transfer_timeout_ms);

/* Backend code is responsible for acquiring stream->lock in their callbacks */
int async_run_stream(struct bladerf_stream *stream,
                     bladerf_channel_layout layout);


/* This function WILL acquire stream->lock before calling backend code.
 *
 * If nonblock=true and no transfers are available, this function shall return
 * BLADERF_ERR_WOULD_BLOCK.
 */
int async_submit_stream_buffer(struct bladerf_stream *stream,
                               void *buffer,
                               size_t *length,
                               unsigned int timeout_ms,
                               bool nonblock);


void async_deinit_stream(struct bladerf_stream *stream);

#endif
