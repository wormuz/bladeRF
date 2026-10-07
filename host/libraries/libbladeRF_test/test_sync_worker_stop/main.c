#include <assert.h>

#include "host_config.h"
#include "streaming/async.h"
#include "streaming/sync_worker.h"

int main(void)
{
    struct bladerf_stream stream = {0};
    struct sync_worker worker = {0};

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
    return 0;
}
