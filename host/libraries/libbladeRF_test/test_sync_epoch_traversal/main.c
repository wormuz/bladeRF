/* Integration test for sample-META epoch filtering through sync_rx(). */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host_config.h"
#include "bladeRF.h"
#include "streaming/sync.h"
#include "streaming/sync_worker.h"
#include "streaming/async.h"
#include "streaming/metadata.h"
#include "board/board.h"
#include "board/bladerf2/common.h"
#include "board/bladerf2/rf_transition_policy.h"

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
    struct bladerf dev;
    uint8_t *buffers[2];
    size_t lengths[2];
    sync_buffer_status states[2];
};

static unsigned int rx_overrun_events;
static unsigned int async_withheld_events;
static uint32_t async_withheld_reason;
static unsigned int sync_withheld_events;
static uint32_t sync_withheld_reason;
static uint32_t sync_withheld_reasons[8];
static bool sync_withheld_timestamp_valid[8];
static uint8_t sync_withheld_epochs[8];
static uint64_t sync_withheld_timestamps[8];
static unsigned int async_overrun_events;
static unsigned int sync_host_data_events;
static unsigned int sync_event_order;
static unsigned int sync_host_data_order;
static unsigned int sync_overrun_order;
static unsigned int sync_withheld_order;

static void note_sync_host_data(struct bladerf *dev,
                                const struct bladerf_metadata *metadata)
{
    assert(dev != NULL);
    assert(metadata_rx_has_epoch_samples(metadata));
    sync_host_data_events++;
    sync_host_data_order = ++sync_event_order;
}

static void note_rx_overrun(struct bladerf *dev)
{
    assert(dev != NULL);
    rx_overrun_events++;
    sync_overrun_order = ++sync_event_order;
}

static void note_async_withheld(struct bladerf *dev, uint32_t reason)
{
    assert(dev != NULL);
    async_withheld_events++;
    async_withheld_reason = reason;
}

static void note_sync_withheld(struct bladerf *dev, uint32_t reason)
{
    assert(dev != NULL);
    sync_withheld_events++;
    sync_withheld_order = ++sync_event_order;
    sync_withheld_reason = reason;
    if (sync_withheld_events <=
        sizeof(sync_withheld_reasons) / sizeof(sync_withheld_reasons[0])) {
        sync_withheld_reasons[sync_withheld_events - 1] = reason;
    }
}

static void note_sync_withheld_at(struct bladerf *dev, uint32_t reason,
                                  uint8_t epoch_id, uint64_t timestamp,
                                  bool timestamp_valid)
{
    note_sync_withheld(dev, reason);
    const unsigned int index = sync_withheld_events - 1;
    if (index < sizeof(sync_withheld_timestamp_valid) /
                sizeof(sync_withheld_timestamp_valid[0])) {
        sync_withheld_timestamp_valid[index] = timestamp_valid;
        sync_withheld_epochs[index] = epoch_id;
        sync_withheld_timestamps[index] = timestamp;
    }
}

static void note_async_overrun(struct bladerf *dev)
{
    assert(dev != NULL);
    async_overrun_events++;
}

static unsigned int async_rx_callbacks;
static unsigned int async_rx_event_wakeups;
static bool allow_async_rx_buffer = true;
static void *async_rejected_replacement;

static void *replace_rejected_async_buffer(void *user_data, void *buffer)
{
    assert(user_data != NULL && buffer != NULL);
    return async_rejected_replacement;
}

static bool validate_async_rx_buffer(struct bladerf *dev,
                                     bladerf_channel_layout layout,
                                     bladerf_format format,
                                     const void *buffer, size_t length)
{
    assert(dev != NULL && buffer != NULL && length > 0);
    assert((layout & BLADERF_DIRECTION_MASK) == BLADERF_RX);
    (void)format;
    return allow_async_rx_buffer;
}

static void *count_async_rx_callback(struct bladerf *dev,
                                    struct bladerf_stream *stream,
                                    struct bladerf_metadata *metadata,
                                    void *samples, size_t num_samples,
                                    void *user_data)
{
    assert(dev != NULL && stream != NULL && metadata != NULL);
    if (num_samples == 0) {
        assert(samples == NULL);
        assert(user_data != NULL);
        async_rx_event_wakeups++;
        return BLADERF_STREAM_REUSE_BUFFER;
    } else {
        assert(samples != NULL);
        async_rx_callbacks++;
    }
    return samples;
}

static const struct board_fns test_board = {
    .rx_stream_overrun = note_rx_overrun,
    .rx_sync_data_valid = note_sync_host_data,
    .rx_async_stream_overrun = note_async_overrun,
    .rx_data_withheld = note_sync_withheld,
    .rx_data_withheld_at = note_sync_withheld_at,
    .rx_async_buffer_valid = validate_async_rx_buffer,
};

static const struct board_fns test_async_board = {
    .rx_stream_overrun = note_rx_overrun,
    .rx_async_stream_overrun = note_async_overrun,
    .rx_data_withheld = note_async_withheld,
    .rx_async_buffer_valid = validate_async_rx_buffer,
};

static void fixture_init(struct fixture *f)
{
    memset(f, 0, sizeof(*f));
    sync_host_data_events = 0;
    sync_event_order = 0;
    sync_host_data_order = 0;
    sync_overrun_order = 0;
    sync_withheld_order = 0;
    sync_withheld_events = 0;
    sync_withheld_reason = 0;
    memset(sync_withheld_reasons, 0, sizeof(sync_withheld_reasons));
    memset(sync_withheld_timestamp_valid, 0,
           sizeof(sync_withheld_timestamp_valid));
    memset(sync_withheld_epochs, 0, sizeof(sync_withheld_epochs));
    memset(sync_withheld_timestamps, 0, sizeof(sync_withheld_timestamps));
    f->buffers[0] = calloc(1, BYTES_PER_BUFFER);
    f->buffers[1] = calloc(1, BYTES_PER_BUFFER);
    assert(f->buffers[0] != NULL && f->buffers[1] != NULL);

    assert(MUTEX_INIT(&f->sync.lock) == 0);
    assert(MUTEX_INIT(&f->sync.buf_mgmt.lock) == 0);
    assert(COND_INIT(&f->sync.buf_mgmt.buf_ready) == 0);

    f->sync.initialized = true;
    f->dev.board = &test_board;
    f->sync.dev = &f->dev;
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

static void test_unsupported_format_event(void)
{
    struct bladerf dev = {0};
    struct bladerf2_board_data *board_data =
        calloc(1, sizeof(*board_data));
    assert(board_data != NULL);
    dev.board_data = board_data;
    assert(MUTEX_INIT(&dev.lock) == 0);
    assert(MUTEX_INIT(&board_data->rx_async_epoch_lock) == 0);
    assert(MUTEX_INIT(&board_data->rf_transition_event_lock) == 0);
    board_data->rf_transition_epoch_contract_enabled = true;
    board_data->rf_transition_epoch_id = 7;

    /* Callback-side publication must not wait for dev->lock. */
    MUTEX_LOCK(&dev.lock);
    bladerf2_rx_format_unsupported(&dev, BLADERF_FORMAT_PACKET_META, true);
    bladerf2_rx_format_unsupported(&dev, BLADERF_FORMAT_PACKET_META, true);
    MUTEX_UNLOCK(&dev.lock);
    assert(board_data->rf_transition_event_count == 1);
    const struct bladerf_rf_event *event =
        &board_data->rf_transition_events[0];
    assert(event->event_type == BLADERF_RF_EVT_RX_FORMAT_UNSUPPORTED);
    assert(event->epoch_id == 7);
    assert(event->flags == BLADERF_FORMAT_PACKET_META);
    assert(event->error_code == BLADERF_ERR_UNSUPPORTED);

    MUTEX_LOCK(&board_data->rx_async_epoch_lock);
    board_data->rx_format_unsupported_reported = false;
    MUTEX_UNLOCK(&board_data->rx_async_epoch_lock);
    bladerf2_rx_format_unsupported(&dev, BLADERF_FORMAT_SC16_Q11, true);
    assert(board_data->rf_transition_event_count == 2);

    board_data->rx_format_unsupported_reported = false;
    board_data->rx_async_data_withheld_active = false;
    bladerf2_rx_format_unsupported(&dev, BLADERF_FORMAT_SC16_Q11, false);
    bladerf2_rx_format_unsupported(&dev, BLADERF_FORMAT_SC16_Q11, false);
    assert(board_data->rf_transition_event_count == 4);
    assert(!board_data->rx_async_data_withheld_active);
    bladerf2_rx_data_withheld(
        &dev, BLADERF_RF_WITHHELD_EPOCH_UNCERTIFIED);
    assert(board_data->rf_transition_event_count == 5);
    assert(board_data->rf_transition_events[4].event_type ==
           BLADERF_RF_EVT_RX_DATA_WITHHELD);
    assert(board_data->rf_transition_events[4].flags ==
           BLADERF_RF_WITHHELD_EPOCH_UNCERTIFIED);

    MUTEX_DESTROY(&board_data->rf_transition_event_lock);
    MUTEX_DESTROY(&board_data->rx_async_epoch_lock);
    MUTEX_DESTROY(&dev.lock);
    free(board_data);
}

static void test_async_data_withheld_event(void)
{
    struct bladerf dev = {0};
    struct bladerf2_board_data *board_data = calloc(1, sizeof(*board_data));
    assert(board_data != NULL);
    dev.board_data = board_data;
    assert(MUTEX_INIT(&dev.lock) == 0);
    assert(MUTEX_INIT(&board_data->rx_async_epoch_lock) == 0);
    assert(MUTEX_INIT(&board_data->rf_transition_event_lock) == 0);
    board_data->rf_transition_epoch_contract_enabled = true;
    board_data->rf_transition_epoch_id = 9;

    /* The callback writer stays independent of dev->lock and coalesces a
     * withheld run into one history event until valid IQ resumes. */
    MUTEX_LOCK(&dev.lock);
    bladerf2_rx_data_withheld(
        &dev, BLADERF_RF_WITHHELD_EPOCH_UNCERTIFIED);
    bladerf2_rx_data_withheld(
        &dev, BLADERF_RF_WITHHELD_EPOCH_OR_TIMESTAMP_MISMATCH);
    MUTEX_UNLOCK(&dev.lock);
    assert(board_data->rf_transition_event_count == 1);
    const struct bladerf_rf_event *event =
        &board_data->rf_transition_events[0];
    assert(event->event_type == BLADERF_RF_EVT_RX_DATA_WITHHELD);
    assert(event->epoch_id == 9);
    assert(event->flags == BLADERF_RF_WITHHELD_EPOCH_UNCERTIFIED);
    assert(event->fpga_state == BLADERF_RF_STATE_RX_DATA_INVALID);

    bladerf2_rx_data_withheld_reset(&dev);
    bladerf2_rx_data_withheld(
        &dev, BLADERF_RF_WITHHELD_EPOCH_OR_TIMESTAMP_MISMATCH);
    assert(board_data->rf_transition_event_count == 2);

    board_data->rf_transition_epoch_contract_enabled = false;
    MUTEX_LOCK(&dev.lock);
    bladerf2_rx_data_withheld(
        &dev, BLADERF_RF_WITHHELD_SHORT_TRANSFER);
    MUTEX_UNLOCK(&dev.lock);
    assert(board_data->rf_transition_event_count == 3);
    event = &board_data->rf_transition_events[2];
    assert(event->event_type == BLADERF_RF_EVT_RX_DATA_WITHHELD);
    assert(event->flags == BLADERF_RF_WITHHELD_SHORT_TRANSFER);
    bladerf2_rx_data_withheld(
        &dev, BLADERF_RF_WITHHELD_USB_OVERFLOW);
    assert(board_data->rf_transition_event_count == 4);
    event = &board_data->rf_transition_events[3];
    assert(event->event_type == BLADERF_RF_EVT_RX_DATA_WITHHELD);
    assert(event->flags == BLADERF_RF_WITHHELD_USB_OVERFLOW);
    MUTEX_LOCK(&dev.lock);
    bladerf2_rx_async_stream_overrun(&dev);
    MUTEX_UNLOCK(&dev.lock);
    assert(board_data->rf_transition_event_count == 5);
    event = &board_data->rf_transition_events[4];
    assert(event->event_type == BLADERF_RF_EVT_RX_STREAM_OVERRUN);
    assert(event->flags == BLADERF_RF_STREAM_STATUS_OVERRUN);

    MUTEX_DESTROY(&board_data->rf_transition_event_lock);
    MUTEX_DESTROY(&board_data->rx_async_epoch_lock);
    MUTEX_DESTROY(&dev.lock);
    free(board_data);
}

static void test_async_timestamp_continuity(void)
{
    uint8_t *buffer = calloc(1, 2 * MSG_BYTES);
    uint64_t next_timestamp = 0;
    assert(buffer != NULL);

    write_msg(buffer, 5000, 7, 10);
    write_msg(buffer + MSG_BYTES, 5000 + MSG_SAMPLES, 7, 20);
    assert(metadata_rx_buffer_epoch_contiguous(
               buffer, 2 * MSG_BYTES, MSG_BYTES, 7, 5000, false, 0,
               MSG_SAMPLES, &next_timestamp) == METADATA_RX_BUFFER_CONTIGUOUS);
    assert(next_timestamp == 5000 + 2 * MSG_SAMPLES);

    write_msg(buffer, next_timestamp, 7, 30);
    write_msg(buffer + MSG_BYTES, next_timestamp + MSG_SAMPLES + 2, 7, 40);
    assert(metadata_rx_buffer_epoch_contiguous(
               buffer, 2 * MSG_BYTES, MSG_BYTES, 7, 5000, true, next_timestamp,
               MSG_SAMPLES, &next_timestamp) ==
           METADATA_RX_BUFFER_DISCONTINUITY);
    assert(next_timestamp == 5000 + 4 * MSG_SAMPLES + 2);

    write_msg(buffer, next_timestamp, 7, 50);
    write_msg(buffer + MSG_BYTES, next_timestamp + MSG_SAMPLES, 7, 60);
    assert(metadata_rx_buffer_epoch_contiguous(
               buffer, 2 * MSG_BYTES, MSG_BYTES, 7, 5000, true, next_timestamp,
               MSG_SAMPLES, &next_timestamp) == METADATA_RX_BUFFER_CONTIGUOUS);

    write_msg(buffer, 9000, 7, 70);
    write_msg(buffer + MSG_BYTES, 9000 + MSG_SAMPLES / 2, 7, 80);
    assert(metadata_rx_buffer_epoch_contiguous(
               buffer, 2 * MSG_BYTES, MSG_BYTES, 7, 9000, false, 0,
               MSG_SAMPLES / 2, &next_timestamp) == METADATA_RX_BUFFER_CONTIGUOUS);
    assert(next_timestamp == 9000 + MSG_SAMPLES);
    free(buffer);
}

int main(void)
{
    assert(sync_rx_should_drop_stale(BLADERF_FORMAT_SC16_Q11, 0));
    assert(sync_rx_should_drop_stale(BLADERF_FORMAT_SC16_Q11_META,
                                     BLADERF_META_FLAG_RX_NOW));
    assert(!sync_rx_should_drop_stale(BLADERF_FORMAT_SC16_Q11_META, 0));
    assert(sync_rx_should_drop_stale(BLADERF_FORMAT_SC8_Q7_META,
                                     BLADERF_META_FLAG_RX_NOW));
    assert(!sync_rx_should_drop_stale(BLADERF_FORMAT_SC8_Q7_META, 0));
    assert(!sync_rx_should_drop_stale(BLADERF_FORMAT_PACKET_META,
                                      BLADERF_META_FLAG_RX_NOW));

    int16_t out[4 * MSG_SAMPLES];
    struct bladerf_metadata meta;

    test_unsupported_format_event();
    test_async_data_withheld_event();
    test_async_timestamp_continuity();


    uint8_t epoch_messages[BYTES_PER_BUFFER];
    assert(metadata_rx_format_has_epoch_tag(BLADERF_FORMAT_SC16_Q11_META));
    assert(metadata_rx_format_has_epoch_tag(BLADERF_FORMAT_SC8_Q7_META));
    assert(!metadata_rx_format_has_epoch_tag(BLADERF_FORMAT_PACKET_META));
    assert(!metadata_rx_format_has_epoch_tag(BLADERF_FORMAT_SC16_Q11));
    assert(metadata_rx_format_allowed_for_epoch_contract(
        false, BLADERF_FORMAT_SC16_Q11));
    assert(metadata_rx_format_allowed_for_epoch_contract(
        true, BLADERF_FORMAT_SC16_Q11_META));
    assert(metadata_rx_format_allowed_for_epoch_contract(
        true, BLADERF_FORMAT_SC8_Q7_META));
    assert(!metadata_rx_format_allowed_for_epoch_contract(
        true, BLADERF_FORMAT_SC16_Q11));
    assert(!metadata_rx_format_allowed_for_epoch_contract(
        true, BLADERF_FORMAT_SC8_Q7));
    assert(!metadata_rx_format_allowed_for_epoch_contract(
        true, BLADERF_FORMAT_PACKET_META));
    assert(!bladerf2_rx_epoch_transition_blocked_by_async_format(false, 1));
    assert(!bladerf2_rx_epoch_transition_blocked_by_async_format(true, 0));
    assert(bladerf2_rx_epoch_transition_blocked_by_async_format(true, 1));
    write_msg(epoch_messages, 5000, 8, 1111);
    write_msg(epoch_messages + MSG_BYTES, 5000 + MSG_SAMPLES, 8, 1222);
    assert(metadata_rx_buffer_matches_epoch(epoch_messages,
                                            sizeof(epoch_messages), MSG_BYTES,
                                            8, 5000));
    assert(!metadata_rx_buffer_matches_epoch(epoch_messages,
                                             sizeof(epoch_messages) - 1,
                                             MSG_BYTES, 8, 5000));
    assert(!metadata_rx_buffer_matches_epoch(epoch_messages,
                                             sizeof(epoch_messages), MSG_BYTES,
                                             9, 5000));
    assert(!metadata_rx_buffer_matches_epoch(epoch_messages,
                                             sizeof(epoch_messages), MSG_BYTES,
                                             8, 5001));
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
    assert(sync_withheld_events == 1);
    assert(sync_withheld_reason ==
           BLADERF_RF_WITHHELD_EPOCH_OR_TIMESTAMP_MISMATCH);
    assert(sync_withheld_timestamp_valid[0]);
    assert(sync_withheld_timestamps[0] == 0);
    assert(sync_withheld_epochs[0] == 6);
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
    /* The returned samples are a valid contiguous prefix; the overrun marks
     * the later rejected message and must not suppress host-data validity. */
    assert(metadata_rx_has_epoch_samples(&meta));
    assert(sync_host_data_events == 1);
    assert(sync_host_data_order < sync_overrun_order);
    assert(sync_overrun_order < sync_withheld_order);
    assert(rx_overrun_events == 1);
    assert(sync_withheld_events == 1);
    assert(sync_withheld_reason ==
           BLADERF_RF_WITHHELD_EPOCH_OR_TIMESTAMP_MISMATCH);
    assert(sync_withheld_timestamp_valid[0]);
    assert(sync_withheld_timestamps[0] == 1000 + MSG_SAMPLES);
    assert(sync_withheld_epochs[0] == 6);
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
    assert(sync_withheld_events == 1);
    assert(sync_withheld_reason ==
           BLADERF_RF_WITHHELD_EPOCH_OR_TIMESTAMP_MISMATCH);
    assert(sync_withheld_timestamp_valid[0]);
    assert(sync_withheld_timestamps[0] == 900);
    assert(sync_withheld_epochs[0] == 7);
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
    sync_withheld_events = 0;
    sync_withheld_reason = 0;
    memset(sync_withheld_timestamp_valid, 0,
           sizeof(sync_withheld_timestamp_valid));
    memset(sync_withheld_epochs, 0, sizeof(sync_withheld_epochs));
    memset(sync_withheld_timestamps, 0, sizeof(sync_withheld_timestamps));
    assert(sync_rx(&f.sync, out, 100, &meta, 1) == BLADERF_ERR_TIMEOUT);
    assert(meta.actual_count == 0);
    assert(sync_withheld_events == 2);
    assert(sync_withheld_reasons[0] ==
           BLADERF_RF_WITHHELD_EPOCH_OR_TIMESTAMP_MISMATCH);
    assert(sync_withheld_reasons[1] == BLADERF_RF_WITHHELD_SYNC_TIMEOUT);
    assert(sync_withheld_timestamp_valid[0]);
    assert(sync_withheld_timestamps[0] == 1100);
    assert(!sync_withheld_timestamp_valid[1]);
    fixture_destroy(&f);

    /* A timeout after previously admitted META data is bounded at the sync
     * parser's current FPGA coordinate, independently of the async cursor. */
    fixture_init(&f);
    write_msg(f.buffers[0], 1000, 7, 1201);
    write_msg(f.buffers[0] + MSG_BYTES, 1000 + MSG_SAMPLES, 7, 1301);
    write_msg(f.buffers[1], 1000 + 2 * MSG_SAMPLES, 7, 1401);
    write_msg(f.buffers[1] + MSG_BYTES, 1000 + 3 * MSG_SAMPLES, 7, 1501);
    receive(&f, out, 2 * MSG_SAMPLES, &meta);
    receive(&f, out, 2 * MSG_SAMPLES, &meta);
    assert(sync_withheld_events == 0);
    memset(&meta, 0, sizeof(meta));
    meta.flags = BLADERF_META_FLAG_RX_NOW;
    assert(sync_rx(&f.sync, out, 100, &meta, 1) == BLADERF_ERR_TIMEOUT);
    assert(meta.actual_count == 0);
    assert(sync_withheld_events == 1);
    assert(sync_withheld_reason == BLADERF_RF_WITHHELD_SYNC_TIMEOUT);
    assert(sync_withheld_timestamp_valid[0]);
    assert(sync_withheld_timestamps[0] == 1000 + 4 * MSG_SAMPLES);
    assert(sync_withheld_epochs[0] == 7);
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
    assert(sync_withheld_events == 1);
    assert(sync_withheld_reason == BLADERF_RF_WITHHELD_EPOCH_UNCERTIFIED);
    assert(!sync_withheld_timestamp_valid[0]);
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

    /* RX_X2 metadata counts interleaved channel samples, while the FPGA
     * timestamp advances once per paired sample. Two adjacent META messages
     * must therefore remain contiguous across the message boundary. */
    fixture_init(&f);
    f.sync.stream_config.layout = BLADERF_RX_X2;
    f.sync.meta.samples_per_ts = 2;
    write_msg(f.buffers[0], 1000, 7, 1444);
    write_msg(f.buffers[0] + MSG_BYTES, 1000 + MSG_SAMPLES / 2, 7,
              (int16_t)(1444 + MSG_SAMPLES));
    memset(&meta, 0, sizeof(meta));
    meta.flags = BLADERF_META_FLAG_RX_NOW;
    assert(sync_rx(&f.sync, out, 2 * MSG_SAMPLES, &meta, 0) == 0);
    assert(meta.actual_count == 2 * MSG_SAMPLES);
    assert(meta.timestamp == 1000);
    assert(meta.rx_epoch_id_valid && meta.rx_epoch_id == 7);
    assert((meta.status & BLADERF_META_STATUS_OVERRUN) == 0);
    assert_marker(out, MSG_SAMPLES, 1444, 0);
    assert_marker(out + 2 * MSG_SAMPLES, MSG_SAMPLES,
                  (int16_t)(1444 + MSG_SAMPLES), 0);
    fixture_destroy(&f);

    /* Replacing a certified RX_X2 parser must poison both interleaved
     * channels until a fresh transition installs the new epoch boundary. */
    fixture_init(&f);
    f.sync.stream_config.layout = BLADERF_RX_X2;
    f.sync.meta.samples_per_ts = 2;
    assert(sync_rx_epoch_require_transition(&f.sync) == 0);
    assert(f.sync.meta.rx_epoch_data_invalidated);
    write_msg(f.buffers[0], 2000, 7, 1555);
    memset(&meta, 0, sizeof(meta));
    meta.flags = BLADERF_META_FLAG_RX_NOW;
    assert(sync_rx(&f.sync, out, 2, &meta, 0) == BLADERF_ERR_WOULD_BLOCK);
    assert(meta.actual_count == 0);
    assert(sync_rx_epoch_set_min_timestamp(&f.sync, 3000, 8) == 0);
    write_msg(f.buffers[0] + MSG_BYTES, 3000, 8, 1666);
    memset(&meta, 0, sizeof(meta));
    meta.flags = BLADERF_META_FLAG_RX_NOW;
    assert(sync_rx(&f.sync, out, 2, &meta, 0) == 0);
    assert(meta.actual_count == 2);
    assert(meta.timestamp == 3000);
    assert(meta.rx_epoch_id_valid && meta.rx_epoch_id == 8);
    assert_marker(out, 2, 1666, 0);
    fixture_destroy(&f);

    /* A USB worker overrun must reach the board event path even when the
     * application selected a sample-only format with no metadata status
     * field. */
    fixture_init(&f);
    f.sync.stream_config.format = BLADERF_FORMAT_SC16_Q11;
    f.sync.stream_config.samples_per_buffer = 2048;
    f.sync.buf_mgmt.overrun_pending = true;
    assert(sync_rx(&f.sync, out, 8, NULL, 0) == BLADERF_ERR_WOULD_BLOCK);
    assert(rx_overrun_events == 2);
    assert(!f.sync.buf_mgmt.overrun_pending);
    fixture_destroy(&f);

    /* The async USB backend recycles incomplete continuous-IQ transfers
     * without exposing their prefix to application callbacks, and publishes
     * the same device event used by sync RX. */
    fixture_init(&f);
    f.dev.board = &test_async_board;
    struct bladerf_stream async_stream = {0};
    struct bladerf_metadata async_meta = {0};
    int16_t async_samples[2048] = {0};
    void *async_buffers[] = { async_samples };
    async_stream.dev = &f.dev;
    async_stream.layout = BLADERF_RX_X2;
    async_stream.format = BLADERF_FORMAT_SC16_Q11_META;
    async_stream.samples_per_buffer = 1024;
    async_stream.buffers = async_buffers;
    async_stream.num_buffers = ARRAY_SIZE(async_buffers);
    async_stream.cb = count_async_rx_callback;
    async_stream.user_data = async_samples;
    int16_t async_replacement[2048] = {0};
    async_rejected_replacement = async_replacement;
    async_stream.rx_buffer_rejected = replace_rejected_async_buffer;
    async_rx_callbacks = 0;
    async_rx_event_wakeups = 0;
    async_withheld_events = 0;
    async_withheld_reason = 0;
    async_overrun_events = 0;
    assert(MUTEX_INIT(&f.dev.lock) == 0);
    MUTEX_LOCK(&f.dev.lock);
    assert(async_rx_process_buffer(&async_stream, &async_meta, async_samples,
                                   sizeof(async_samples) / 2) ==
           async_replacement);
    MUTEX_UNLOCK(&f.dev.lock);
    assert(MUTEX_DESTROY(&f.dev.lock) == 0);
    assert(rx_overrun_events == 2);
    assert(async_overrun_events == 1);
    assert(async_rx_callbacks == 0);
    assert(async_rx_event_wakeups == 1);
    assert(async_withheld_events == 1);
    assert(async_withheld_reason == BLADERF_RF_WITHHELD_SHORT_TRANSFER);
    assert(MUTEX_INIT(&f.dev.lock) == 0);
    MUTEX_LOCK(&f.dev.lock);
    assert(async_rx_process_buffer(&async_stream, &async_meta, async_samples,
                                   sizeof(async_samples) / 2) ==
           async_replacement);
    MUTEX_UNLOCK(&f.dev.lock);
    assert(MUTEX_DESTROY(&f.dev.lock) == 0);
    assert(async_rx_event_wakeups == 2);
    assert(async_withheld_events == 2);
    assert(async_overrun_events == 2);
    async_withheld_events = 0;
    async_withheld_reason = 0;
    async_rx_event_wakeups = 0;
    async_stream.cb = count_async_rx_callback;
    async_stream.user_data = async_samples;
    assert(MUTEX_INIT(&f.dev.lock) == 0);
    MUTEX_LOCK(&f.dev.lock);
    async_notify_rx_transport_failure(
        &async_stream, BLADERF_RF_WITHHELD_USB_OVERFLOW);
    MUTEX_UNLOCK(&f.dev.lock);
    assert(MUTEX_DESTROY(&f.dev.lock) == 0);
    assert(async_withheld_events == 1);
    assert(async_withheld_reason == BLADERF_RF_WITHHELD_USB_OVERFLOW);
    assert(async_overrun_events == 3);
    assert(async_rx_event_wakeups == 1);
    allow_async_rx_buffer = false;
    assert(async_rx_process_buffer(&async_stream, &async_meta, async_samples,
                                   sizeof(async_samples)) ==
           async_replacement);
    assert(async_rx_callbacks == 0);
    assert(async_rx_event_wakeups == 1);
    assert(async_rx_process_buffer(&async_stream, &async_meta, async_samples,
                                   sizeof(async_samples)) ==
           async_replacement);
    assert(async_rx_event_wakeups == 1);
    allow_async_rx_buffer = true;
    async_stream.layout = BLADERF_RX_X1;
    assert(async_rx_process_buffer(&async_stream, &async_meta, async_samples,
                                   sizeof(async_samples)) == async_samples);
    assert(async_rx_callbacks == 1);
    assert(async_rx_event_wakeups == 1);
    assert(rx_overrun_events == 2);
    allow_async_rx_buffer = false;
    async_stream.format = BLADERF_FORMAT_PACKET_META;
    assert(async_rx_process_buffer(&async_stream, &async_meta, async_samples,
                                   sizeof(async_samples)) ==
           async_replacement);
    assert(async_rx_callbacks == 1);
    assert(async_rx_event_wakeups == 2);
    allow_async_rx_buffer = true;
    assert(async_rx_process_buffer(&async_stream, &async_meta, async_samples,
                                   sizeof(async_samples)) == async_samples);
    assert(async_rx_callbacks == 2);
    assert(async_rx_event_wakeups == 2);
    async_stream.layout = BLADERF_TX_X2;
    async_notify_rx_overrun(&async_stream);
    async_notify_rx_overrun(NULL);
    assert(rx_overrun_events == 2);
    fixture_destroy(&f);

    return 0;
}
