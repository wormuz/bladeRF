#include <assert.h>
#include <stdlib.h>

#include "host_config.h"
#include "streaming/async.h"
#include "streaming/sync_worker.h"

static void *startup_waiting_worker(void *arg)
{
    struct sync_worker *worker = arg;

    MUTEX_LOCK(&worker->request_lock);
    while ((worker->requests & SYNC_WORKER_STOP) == 0) {
        COND_WAIT(&worker->requests_pending, &worker->request_lock);
    }
    MUTEX_UNLOCK(&worker->request_lock);

    MUTEX_LOCK(&worker->state_lock);
    worker->state = SYNC_WORKER_STATE_STOPPED;
    COND_SIGNAL(&worker->state_changed);
    MUTEX_UNLOCK(&worker->state_lock);
    return NULL;
}

int main(void)
{
    struct bladerf_stream stream = {0};
    struct sync_worker worker = {0};

    assert(sync_worker_transfer_timeout_ms(BLADERF_RX,
               BLADERF_FORMAT_SC16_Q11_META, 1000) ==
           SYNC_RX_META_TRANSITION_TIMEOUT_MS);
    assert(sync_worker_transfer_timeout_ms(BLADERF_RX,
               BLADERF_FORMAT_SC16_Q11_META, 3000) ==
           SYNC_RX_META_TRANSITION_TIMEOUT_MS);
    assert(sync_worker_transfer_timeout_ms(BLADERF_RX,
               BLADERF_FORMAT_SC16_Q11_META, 5000) ==
           SYNC_RX_META_TRANSITION_TIMEOUT_MS);
    assert(sync_worker_transfer_timeout_ms(BLADERF_RX,
               BLADERF_FORMAT_SC16_Q11_META, 6000) == 6000);
    assert(sync_worker_transfer_timeout_ms(BLADERF_RX,
               BLADERF_FORMAT_SC16_Q11_META, 7000) == 7000);
    assert(sync_worker_transfer_timeout_ms(BLADERF_TX,
               BLADERF_FORMAT_SC16_Q11_META, 1000) == 1000);
    assert(sync_worker_transfer_timeout_ms(BLADERF_RX,
               BLADERF_FORMAT_SC16_Q11, 1000) == 1000);

    stream.state = STREAM_RUNNING;
    stream.layout = BLADERF_RX_X1;
    worker.stream = &stream;

    assert(MUTEX_INIT(&stream.lock) == 0);
    assert(MUTEX_INIT(&worker.request_lock) == 0);
    assert(COND_INIT(&worker.requests_pending) == 0);

    sync_worker_submit_request(&worker, SYNC_WORKER_STOP);

    assert(worker.requests & SYNC_WORKER_STOP);
    assert(stream.state == STREAM_SHUTTING_DOWN);

    assert(MUTEX_DESTROY(&worker.request_lock) == 0);
    assert(MUTEX_DESTROY(&stream.lock) == 0);

    /* Startup can time out before the worker reports IDLE. Its state must
     * stay alive until STOP is observed and the created thread is joined. */
    {
        struct sync_worker *startup_worker =
            calloc(1, sizeof(*startup_worker));
        assert(startup_worker != NULL);
        startup_worker->state = SYNC_WORKER_STATE_STARTUP;
        assert(MUTEX_INIT(&startup_worker->state_lock) == 0);
        assert(MUTEX_INIT(&startup_worker->request_lock) == 0);
        assert(COND_INIT(&startup_worker->state_changed) == 0);
        assert(COND_INIT(&startup_worker->requests_pending) == 0);
        assert(THREAD_CREATE(&startup_worker->thread,
                             startup_waiting_worker, startup_worker) ==
               THREAD_SUCCESS);

        /* Request stop immediately, including the race where the task has
         * not yet executed its first set_state(IDLE). */
        sync_worker_request_stop_and_join(startup_worker, NULL, NULL);
        assert(startup_worker->state == SYNC_WORKER_STATE_STOPPED);
        assert(COND_DESTROY(&startup_worker->requests_pending) == 0);
        assert(COND_DESTROY(&startup_worker->state_changed) == 0);
        assert(MUTEX_DESTROY(&startup_worker->request_lock) == 0);
        assert(MUTEX_DESTROY(&startup_worker->state_lock) == 0);
        free(startup_worker);
    }

    /* Invalid transfers are recycled by async.c without passing through the
     * normal sample callback. Their ring sequence must still be retired so a
     * later valid callback cannot wait forever behind a hole. */
    {
        struct bladerf_sync sync = {0};
        struct buffer_mgmt buffers = {0};
        void *ring[4];
        uint32_t sequences[4] = { UINT32_MAX, 1, 2, 3 };
        bool dropped[4] = { false };
        sync_buffer_status states[4] = {
            SYNC_BUFFER_EMPTY, SYNC_BUFFER_IN_FLIGHT,
            SYNC_BUFFER_IN_FLIGHT, SYNC_BUFFER_IN_FLIGHT,
        };
        unsigned char storage[4][8] = {{0}};
        for (unsigned int i = 0; i < 4; ++i) {
            ring[i] = storage[i];
        }

        sync.stream_config.layout = BLADERF_RX_X1;
        sync.initialized = true;
        sync.buf_mgmt = buffers;
        sync.buf_mgmt.buffers = ring;
        sync.buf_mgmt.status = states;
        sync.buf_mgmt.buffer_seq = sequences;
        sync.buf_mgmt.buffer_dropped = dropped;
        sync.buf_mgmt.num_buffers = 4;
        sync.buf_mgmt.expected_seq = 1;
        sync.buf_mgmt.next_seq = 4;
        sync.buf_mgmt.reorder_limit = 3;
        assert(MUTEX_INIT(&sync.buf_mgmt.lock) == 0);
        assert(COND_INIT(&sync.buf_mgmt.buf_ready) == 0);

        /* A withheld completion retires its sequence and hands async.c the
         * next producer slot, matching the normal RX callback rotation. */
        void *replacement = sync_worker_rx_buffer_rejected(&sync, ring[3]);
        assert(replacement == ring[0]);
        assert(dropped[3]);
        assert(sync.buf_mgmt.expected_seq == 1);
        assert(sync.buf_mgmt.reorder_len == 1);
        assert(sync.buf_mgmt.reorder[0].dropped);
        assert(sequences[0] == 4);
        assert(sync.buf_mgmt.prod_i == 1);
        assert(states[0] == SYNC_BUFFER_IN_FLIGHT);
        assert(states[3] == SYNC_BUFFER_EMPTY);
        assert(!sync.buf_mgmt.overrun_pending);
        assert(MUTEX_DESTROY(&sync.buf_mgmt.lock) == 0);
    }

    /* If an epoch fence spans a full ring wrap, cons_i must be re-anchored to
     * the oldest live sequence rather than wrapping back to a stale slot. */
    {
        struct bladerf_sync sync = {0};
        struct buffer_mgmt buffers = {0};
        uint32_t sequences[4] = { 10, 11, 12, 13 };
        bool dropped[4] = { true, true, true, true };
        sync_buffer_status states[4] = {
            SYNC_BUFFER_IN_FLIGHT, SYNC_BUFFER_EMPTY,
            SYNC_BUFFER_IN_FLIGHT, SYNC_BUFFER_FULL,
        };

        sync.initialized = true;
        sync.buf_mgmt = buffers;
        sync.buf_mgmt.status = states;
        sync.buf_mgmt.buffer_seq = sequences;
        sync.buf_mgmt.buffer_dropped = dropped;
        sync.buf_mgmt.num_buffers = 4;
        sync.buf_mgmt.cons_i = 0;
        sync.buf_mgmt.expected_seq = 12;
        assert(MUTEX_INIT(&sync.buf_mgmt.lock) == 0);
        MUTEX_LOCK(&sync.buf_mgmt.lock);
        unsigned int skipped = 0;
        while (skipped < sync.buf_mgmt.num_buffers &&
               sync.buf_mgmt.buffer_dropped[sync.buf_mgmt.cons_i]) {
            sync.buf_mgmt.buffer_dropped[sync.buf_mgmt.cons_i] = false;
            sync.buf_mgmt.cons_i =
                (sync.buf_mgmt.cons_i + 1) % sync.buf_mgmt.num_buffers;
            skipped++;
        }
        assert(skipped == sync.buf_mgmt.num_buffers);
        assert(sync_worker_reanchor_rx_consumer_to_expected(&sync));
        assert(sync.buf_mgmt.cons_i == 2);
        assert(sync.buf_mgmt.partial_off == 0);
        MUTEX_UNLOCK(&sync.buf_mgmt.lock);
        assert(MUTEX_DESTROY(&sync.buf_mgmt.lock) == 0);
    }

    /* A completed buffer outside the reorder window is forwarded only with
     * an explicit discontinuity pending for the sync reader. */
    {
        struct bladerf_sync sync = {0};
        sync.buf_mgmt.expected_seq = 10;
        sync.buf_mgmt.reorder_limit = 2;
        assert(MUTEX_INIT(&sync.buf_mgmt.lock) == 0);
        assert(COND_INIT(&sync.buf_mgmt.buf_ready) == 0);

        MUTEX_LOCK(&sync.buf_mgmt.lock);
        assert(sync_worker_rx_reorder_buffer(&sync, 11, 0, 64));
        assert(sync.buf_mgmt.reorder_len == 1);
        assert(!sync.buf_mgmt.overrun_pending);
        assert(!sync.buf_mgmt.stale_pending);

        assert(!sync_worker_rx_reorder_buffer(&sync, 13, 1, 64));
        assert(sync.buf_mgmt.overrun_pending);
        assert(sync.buf_mgmt.stale_pending);
        MUTEX_UNLOCK(&sync.buf_mgmt.lock);

        assert(MUTEX_DESTROY(&sync.buf_mgmt.lock) == 0);
    }

    /* Starting a new RF epoch must release already completed old-epoch
     * buffers before the consumer resumes. This is intentional epoch
     * invalidation, not a transport overrun. */
    {
        struct bladerf_sync sync = {0};
        void *ring[4];
        size_t lengths[4] = {64, 32, 0, 64};
        bool dropped[4] = { false };
        sync_buffer_status states[4] = {
            SYNC_BUFFER_FULL, SYNC_BUFFER_PARTIAL,
            SYNC_BUFFER_IN_FLIGHT, SYNC_BUFFER_FULL,
        };
        unsigned char storage[4][64] = {{0}};
        for (unsigned int i = 0; i < 4; ++i) {
            ring[i] = storage[i];
        }

        sync.initialized = true;
        sync.stream_config.layout = BLADERF_RX_X1;
        sync.buf_mgmt.buffers = ring;
        sync.buf_mgmt.status = states;
        sync.buf_mgmt.actual_lengths = lengths;
        sync.buf_mgmt.buffer_dropped = dropped;
        sync.buf_mgmt.num_buffers = 4;
        sync.buf_mgmt.prod_i = 2;
        sync.buf_mgmt.cons_i = 0;
        assert(MUTEX_INIT(&sync.lock) == 0);
        assert(MUTEX_INIT(&sync.buf_mgmt.lock) == 0);

        MUTEX_LOCK(&sync.lock);
        sync_worker_discard_rx_epoch(&sync);
        MUTEX_UNLOCK(&sync.lock);

        assert(states[0] == SYNC_BUFFER_EMPTY);
        assert(states[1] == SYNC_BUFFER_EMPTY);
        assert(states[2] == SYNC_BUFFER_IN_FLIGHT);
        assert(states[3] == SYNC_BUFFER_EMPTY);
        assert(dropped[0] && dropped[1] && dropped[3]);
        assert(lengths[0] == 0 && lengths[1] == 0 && lengths[3] == 0);
        assert(!sync.buf_mgmt.overrun_pending);
        assert(!sync.buf_mgmt.stale_pending);

        assert(MUTEX_DESTROY(&sync.buf_mgmt.lock) == 0);
        assert(MUTEX_DESTROY(&sync.lock) == 0);
    }

    /* Rejected completions may arrive out of order around the sequence wrap.
     * Once the missing head sequence is retired, queued tombstones must flush
     * in order instead of leaving expected_seq permanently behind. */
    {
        struct bladerf_sync sync = {0};
        struct buffer_mgmt buffers = {0};
        void *ring[4];
        uint32_t sequences[4] = { UINT32_MAX, 0, 1, 2 };
        bool dropped[4] = { false };
        sync_buffer_status states[4] = {
            SYNC_BUFFER_IN_FLIGHT, SYNC_BUFFER_IN_FLIGHT,
            SYNC_BUFFER_IN_FLIGHT, SYNC_BUFFER_IN_FLIGHT,
        };
        unsigned char storage[4][8] = {{0}};
        for (unsigned int i = 0; i < 4; ++i) {
            ring[i] = storage[i];
        }

        sync.stream_config.layout = BLADERF_RX_X1;
        sync.buf_mgmt = buffers;
        sync.buf_mgmt.buffers = ring;
        sync.buf_mgmt.status = states;
        sync.buf_mgmt.buffer_seq = sequences;
        sync.buf_mgmt.buffer_dropped = dropped;
        sync.buf_mgmt.num_buffers = 4;
        sync.buf_mgmt.expected_seq = UINT32_MAX;
        sync.buf_mgmt.next_seq = 3;
        sync.buf_mgmt.reorder_limit = 3;
        assert(MUTEX_INIT(&sync.buf_mgmt.lock) == 0);
        assert(COND_INIT(&sync.buf_mgmt.buf_ready) == 0);

        (void)sync_worker_rx_buffer_rejected(&sync, ring[0]);
        assert(sync.buf_mgmt.expected_seq == 0);

        /* Sequence 1 is rejected before sequence 0; it must wait as a
         * tombstone, including across UINT32_MAX -> 0 wrap. */
        (void)sync_worker_rx_buffer_rejected(&sync, ring[2]);
        assert(sync.buf_mgmt.expected_seq == 0);
        assert(sync.buf_mgmt.reorder_len == 1);
        assert(sync.buf_mgmt.reorder[0].seq == 1);
        assert(sync.buf_mgmt.reorder[0].dropped);

        (void)sync_worker_rx_buffer_rejected(&sync, ring[1]);
        assert(sync.buf_mgmt.expected_seq == 2);
        assert(sync.buf_mgmt.reorder_len == 0);

        assert(MUTEX_DESTROY(&sync.buf_mgmt.lock) == 0);
    }

    /* A stalled head transfer can leave enough later completions to fill the
     * bounded tombstone queue. Overflow must be an observable discontinuity,
     * not a warning that leaves sync_rx asleep behind an unretirable hole. */
    {
        enum { NUM_BUFFERS = SYNC_RX_MAX_REORDER + 4 };
        struct bladerf_sync sync = {0};
        void *ring[NUM_BUFFERS];
        uint32_t sequences[NUM_BUFFERS];
        bool dropped[NUM_BUFFERS] = { false };
        sync_buffer_status states[NUM_BUFFERS];
        unsigned char storage[NUM_BUFFERS][8] = {{0}};

        for (unsigned int i = 0; i < NUM_BUFFERS; ++i) {
            ring[i] = storage[i];
            sequences[i] = i;
            states[i] = SYNC_BUFFER_IN_FLIGHT;
        }
        states[NUM_BUFFERS - 1] = SYNC_BUFFER_EMPTY;

        sync.stream_config.layout = BLADERF_RX_X1;
        sync.buf_mgmt.buffers = ring;
        sync.buf_mgmt.status = states;
        sync.buf_mgmt.buffer_seq = sequences;
        sync.buf_mgmt.buffer_dropped = dropped;
        sync.buf_mgmt.num_buffers = NUM_BUFFERS;
        sync.buf_mgmt.expected_seq = 0; /* Intentionally stalled head. */
        sync.buf_mgmt.next_seq = NUM_BUFFERS;
        sync.buf_mgmt.prod_i = NUM_BUFFERS - 1;
        sync.buf_mgmt.reorder_limit = NUM_BUFFERS - 1;
        assert(MUTEX_INIT(&sync.buf_mgmt.lock) == 0);
        assert(COND_INIT(&sync.buf_mgmt.buf_ready) == 0);

        for (unsigned int i = 1; i <= SYNC_RX_MAX_REORDER + 1; ++i) {
            (void)sync_worker_rx_buffer_rejected(&sync, ring[i]);
        }

        assert(sync.buf_mgmt.reorder_len == SYNC_RX_MAX_REORDER);
        assert(sync.buf_mgmt.overrun_pending);
        assert(sync.buf_mgmt.stale_pending);

        assert(MUTEX_DESTROY(&sync.buf_mgmt.lock) == 0);
    }
    return 0;
}
