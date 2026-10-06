/* Integration test for sample-META epoch filtering through sync_rx(). */
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "host_config.h"
#include "streaming/sync.h"
#include "streaming/sync_worker.h"
#include "streaming/metadata.h"

#define MSG_BYTES 8192u
#define MSG_SAMPLES ((MSG_BYTES - METADATA_HEADER_SIZE) / 4u)
#define MSGS_PER_BUFFER 2u
#define SAMPLES_PER_BUFFER (2048u * MSGS_PER_BUFFER)
#define BYTES_PER_BUFFER (MSG_BYTES * MSGS_PER_BUFFER)

/* sync_rx() never calls these when the fixture begins with ready buffers.
 * Definitions are still needed because the production parser has other state
 * branches in the same function. */
sync_worker_state sync_worker_get_state(struct sync_worker *worker, int *err)
{
    (void)worker;
    if (err != NULL) {
        *err = 0;
    }
    return SYNC_WORKER_STATE_RUNNING;
}

void sync_worker_submit_request(struct sync_worker *worker, unsigned int req)
{
    (void)worker;
    (void)req;
}

int sync_worker_wait_for_state(struct sync_worker *worker,
                               sync_worker_state state,
                               unsigned int timeout_ms)
{
    (void)worker;
    (void)state;
    (void)timeout_ms;
    return 0;
}

static void write_msg(uint8_t *msg, uint64_t timestamp, uint8_t epoch_id,
                      int16_t marker)
{
    uint32_t tag = HOST_TO_LE32(METADATA_RX_EPOCH_TAG_VALID | epoch_id);
    uint64_t stamp = HOST_TO_LE64(timestamp);
    memcpy(msg + METADATA_RESV_OFFSET, &tag, sizeof(tag));
    memcpy(msg + METADATA_TIMESTAMP_OFFSET, &stamp, sizeof(stamp));
    memset(msg + METADATA_FLAGS_OFFSET, 0, METADATA_FLAGS_SIZE);

    int16_t *iq = (int16_t *)(msg + METADATA_HEADER_SIZE);
    for (unsigned int i = 0; i < MSG_SAMPLES; ++i) {
        iq[2 * i] = (int16_t)(marker + i);
        iq[2 * i + 1] = (int16_t)-(marker + i);
    }
}

struct fixture {
    struct bladerf_sync sync;
    uint8_t *buffers[2];
    size_t lengths[2];
    sync_buffer_status states[2];
};

static void fixture_init(struct fixture *f)
{
    memset(f, 0, sizeof(*f));
    f->buffers[0] = calloc(1, BYTES_PER_BUFFER);
    f->buffers[1] = calloc(1, BYTES_PER_BUFFER);
    assert(f->buffers[0] != NULL && f->buffers[1] != NULL);

    assert(MUTEX_INIT(&f->sync.lock) == 0);
    assert(MUTEX_INIT(&f->sync.buf_mgmt.lock) == 0);
    assert(COND_INIT(&f->sync.buf_mgmt.buf_ready) == 0);

    f->sync.initialized = true;
    f->sync.state = SYNC_STATE_WAIT_FOR_BUFFER;
    f->sync.stream_config.format = BLADERF_FORMAT_SC16_Q11_META;
    f->sync.stream_config.layout = BLADERF_RX_X1;
    f->sync.stream_config.samples_per_buffer = SAMPLES_PER_BUFFER;
    f->sync.stream_config.bytes_per_sample = 4;
    f->sync.buf_mgmt.buffers = (void **)f->buffers;
    f->sync.buf_mgmt.actual_lengths = f->lengths;
    f->sync.buf_mgmt.status = f->states;
    f->sync.buf_mgmt.num_buffers = 2;
    f->sync.buf_mgmt.cons_i = 0;
    f->states[0] = SYNC_BUFFER_FULL;
    f->states[1] = SYNC_BUFFER_FULL;

    f->sync.meta.state = SYNC_META_STATE_HEADER;
    f->sync.meta.msg_size = MSG_BYTES;
    f->sync.meta.msg_per_buf = MSGS_PER_BUFFER;
    f->sync.meta.samples_per_msg = MSG_SAMPLES;
    f->sync.meta.samples_per_ts = 1;
    f->sync.meta.rx_epoch_boundary_enabled = true;
    f->sync.meta.rx_epoch_min_timestamp = 1000;
    f->sync.meta.rx_epoch_id_filter_enabled = true;
    f->sync.meta.rx_epoch_expected_id = 7;
    f->lengths[0] = f->lengths[1] = BYTES_PER_BUFFER;
}

static void fixture_destroy(struct fixture *f)
{
    assert(pthread_mutex_destroy(&f->sync.lock) == 0);
    assert(pthread_mutex_destroy(&f->sync.buf_mgmt.lock) == 0);
    assert(pthread_cond_destroy(&f->sync.buf_mgmt.buf_ready) == 0);
    free(f->buffers[0]);
    free(f->buffers[1]);
}

static void receive(struct fixture *f, int16_t *out, unsigned int count,
                    struct bladerf_metadata *meta)
{
    memset(meta, 0, sizeof(*meta));
    meta->flags = BLADERF_META_FLAG_RX_NOW;
    assert(sync_rx(&f->sync, out, count, meta, 0) == 0);
    assert(meta->actual_count == count);
}

static void assert_marker(const int16_t *iq, unsigned int count,
                          int16_t marker, unsigned int sample_offset)
{
    for (unsigned int i = 0; i < count; ++i) {
        const int16_t expected = (int16_t)(marker + sample_offset + i);
        assert(iq[2 * i] == expected);
        assert(iq[2 * i + 1] == (int16_t)-expected);
    }
}

int main(void)
{
    int16_t out[2 * MSG_SAMPLES];
    struct bladerf_metadata meta;
    struct fixture f;

    /* Traverse two messages in one USB buffer: discard the stale epoch at
     * the head, then return only matching epoch samples. */
    fixture_init(&f);
    write_msg(f.buffers[0], 0, 6, 111);
    write_msg(f.buffers[0] + MSG_BYTES, MSG_SAMPLES, 7, 222);
    receive(&f, out, MSG_SAMPLES, &meta);
    assert(meta.timestamp == MSG_SAMPLES);
    assert(meta.rx_epoch_id_valid && meta.rx_epoch_id == 7);
    assert((meta.status & BLADERF_META_STATUS_OVERRUN) == 0);
    assert_marker(out, MSG_SAMPLES, 222, 0);
    fixture_destroy(&f);

    /* A stale message after copied current-epoch data returns the valid
     * prefix with overrun. The next sync_rx() call drops that message and
     * crosses to the following buffer before exposing more IQ. */
    fixture_init(&f);
    write_msg(f.buffers[0], 1000, 7, 333);
    write_msg(f.buffers[0] + MSG_BYTES, 1000 + MSG_SAMPLES, 6, 444);
    write_msg(f.buffers[1], 1000 + 2 * MSG_SAMPLES, 7, 555);
    memset(&meta, 0, sizeof(meta));
    meta.flags = BLADERF_META_FLAG_RX_NOW;
    assert(sync_rx(&f.sync, out, MSG_SAMPLES + 1, &meta, 0) == 0);
    assert(meta.actual_count == MSG_SAMPLES);
    assert(meta.timestamp == 1000);
    assert(meta.rx_epoch_id_valid && meta.rx_epoch_id == 7);
    assert(meta.status & BLADERF_META_STATUS_OVERRUN);
    assert_marker(out, MSG_SAMPLES, 333, 0);

    receive(&f, out, MSG_SAMPLES, &meta);
    assert(meta.timestamp == 1000 + 2 * MSG_SAMPLES);
    assert(meta.rx_epoch_id_valid && meta.rx_epoch_id == 7);
    assert_marker(out, MSG_SAMPLES, 555, 0);
    fixture_destroy(&f);

    /* Enforce the first-valid boundary inside a matching message and report
     * the timestamp of the first returned sample, after the skipped prefix. */
    fixture_init(&f);
    write_msg(f.buffers[0], 900, 7, 666);
    write_msg(f.buffers[0] + MSG_BYTES, 900 + MSG_SAMPLES, 7, 777);
    receive(&f, out, 100, &meta);
    assert(meta.timestamp == 1000);
    assert(meta.rx_epoch_id_valid && meta.rx_epoch_id == 7);
    assert_marker(out, 100, 666, 100);
    fixture_destroy(&f);

    /* A retune invalidates the remainder of a message that the parser had
     * already classified under the formerly valid epoch. */
    fixture_init(&f);
    write_msg(f.buffers[0], 1000, 7, 811);
    write_msg(f.buffers[0] + MSG_BYTES, 1000 + MSG_SAMPLES, 7, 911);
    write_msg(f.buffers[1], 1000 + 2 * MSG_SAMPLES, 7, 1011);
    write_msg(f.buffers[1] + MSG_BYTES, 1000 + 3 * MSG_SAMPLES, 7, 1111);
    receive(&f, out, 100, &meta);
    assert(meta.rx_epoch_id_valid && meta.rx_epoch_id == 7);
    assert(sync_rx_epoch_invalidate(&f.sync) == 0);
    assert(sync_rx_epoch_expect_id(&f.sync, 8) == 0);
    assert(sync_rx_epoch_set_min_timestamp(&f.sync, 4000, 8) == 0);
    /* Even though successful-boundary installation clears the invalidation
     * latch, the already-open old message remains marked for discard. */
    memset(&meta, 0, sizeof(meta));
    meta.flags = BLADERF_META_FLAG_RX_NOW;
    assert(sync_rx(&f.sync, out, 100, &meta, 1) == BLADERF_ERR_TIMEOUT);
    assert(meta.actual_count == 0);
    fixture_destroy(&f);

    /* A transition poisons the entire parser epoch before ARM. Even a packet
     * carrying the soon-to-be expected ID cannot escape until wait() confirms
     * RX_EPOCH_VALID and installs the timestamp boundary. */
    fixture_init(&f);
    assert(sync_rx_epoch_expect_id(&f.sync, 8) == 0);
    write_msg(f.buffers[0], 0, 7, 888);
    write_msg(f.buffers[0] + MSG_BYTES, 100, 8, 999);
    write_msg(f.buffers[1], 200, 8, 1001);
    memset(&meta, 0, sizeof(meta));
    meta.flags = BLADERF_META_FLAG_RX_NOW;
    assert(sync_rx(&f.sync, out, 100, &meta, 1) == BLADERF_ERR_WOULD_BLOCK);
    assert(meta.actual_count == 0);
    assert(f.sync.meta.rx_epoch_data_invalidated);
    fixture_destroy(&f);

    /* Only the exact successful boundary clears invalidation; old queued
     * packets remain suppressed and matching samples after it are returned. */
    fixture_init(&f);
    assert(sync_rx_epoch_expect_id(&f.sync, 8) == 0);
    assert(sync_rx_epoch_set_min_timestamp(&f.sync, 100, 8) == 0);
    write_msg(f.buffers[0], 0, 7, 888);
    write_msg(f.buffers[0] + MSG_BYTES, 100, 8, 999);
    receive(&f, out, 100, &meta);
    assert(meta.timestamp == 100);
    assert(meta.rx_epoch_id_valid && meta.rx_epoch_id == 8);
    assert_marker(out, 100, 999, 0);

    fixture_destroy(&f);

    /* Timestamp continuity resets at a new certified epoch. Stale queued
     * packets are drained without becoming the baseline for the first
     * matching packet, whose timestamp is unrelated to the old epoch. */
    fixture_init(&f);
    f.sync.meta.have_timestamp = true;
    f.sync.meta.curr_timestamp = 1000 + MSG_SAMPLES;
    assert(sync_rx_epoch_expect_id(&f.sync, 8) == 0);
    assert(sync_rx_epoch_set_min_timestamp(&f.sync, 5000, 8) == 0);
    write_msg(f.buffers[0], 2000, 7, 1222);
    write_msg(f.buffers[0] + MSG_BYTES, 5000, 8, 1333);
    receive(&f, out, 100, &meta);
    assert(meta.timestamp == 5000);
    assert(meta.rx_epoch_id_valid && meta.rx_epoch_id == 8);
    assert((meta.status & BLADERF_META_STATUS_OVERRUN) == 0);
    assert_marker(out, 100, 1333, 0);
    fixture_destroy(&f);

    return 0;
}
