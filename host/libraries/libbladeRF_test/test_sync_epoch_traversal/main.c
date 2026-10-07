/* Integration test for sample-META epoch filtering through sync_rx(). */
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

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
static uint32_t last_rx_overrun_source_flags;
static unsigned int rx_worker_overrun_events;
static uint32_t last_rx_worker_overrun_source_flags;
static unsigned int async_withheld_events;
static uint32_t async_withheld_reason;
static unsigned int sync_withheld_events;
static uint32_t sync_withheld_reason;
static uint32_t sync_withheld_reasons[8];
static bool sync_withheld_timestamp_valid[8];
static uint8_t sync_withheld_epochs[8];
static uint64_t sync_withheld_timestamps[8];
static atomic_bool sync_withheld_observed;
static unsigned int async_overrun_events;
static unsigned int async_fault_order;
static unsigned int async_fault_withheld_order;
static unsigned int async_fault_overrun_order;
static unsigned int async_fault_rejected_order;
static unsigned int async_fault_callback_order;
static unsigned int sync_host_data_events;
static unsigned int sync_event_order;
static unsigned int sync_host_data_order;
static unsigned int sync_overrun_order;
static unsigned int sync_withheld_order;
static bool allow_sync_channel_selection = true;

static void test_rx_channel_mask_runtime_policy(void)
{
    assert(bladerf2_rx_channel_mask_observation(
               false, true, 0x1, 0x1) ==
           BLADERF2_RX_CHANNEL_MASK_UNAVAILABLE);
    assert(bladerf2_rx_channel_mask_observation(
               true, false, 0x1, 0x1) ==
           BLADERF2_RX_CHANNEL_MASK_UNAVAILABLE);

    /* RX1, RX2, and paired RX_X2 all share the same mask observation rule. */
    assert(bladerf2_rx_channel_mask_observation(
               true, true, 0x1, 0x1) == BLADERF2_RX_CHANNEL_MASK_MATCH);
    assert(bladerf2_rx_channel_mask_observation(
               true, true, 0x2, 0x2) == BLADERF2_RX_CHANNEL_MASK_MATCH);
    assert(bladerf2_rx_channel_mask_observation(
               true, true, 0x3, 0x3) == BLADERF2_RX_CHANNEL_MASK_MATCH);
    assert(bladerf2_rx_channel_mask_observation(
               true, true, 0x1, 0x0) == BLADERF2_RX_CHANNEL_MASK_CHANGED);
    assert(bladerf2_rx_channel_mask_observation(
               true, true, 0x2, 0x0) == BLADERF2_RX_CHANNEL_MASK_CHANGED);
    assert(bladerf2_rx_channel_mask_observation(
               true, true, 0x3, 0x1) == BLADERF2_RX_CHANNEL_MASK_CHANGED);
}

static bool validate_sync_channel_selection(
    struct bladerf *dev, bladerf_channel_layout layout)
{
    assert(dev != NULL);
    assert(layout == BLADERF_RX_X1 || layout == BLADERF_RX_X2);
    return allow_sync_channel_selection;
}

static void note_sync_host_data(struct bladerf *dev,
                                const struct bladerf_metadata *metadata,
                                bladerf_channel_layout layout)
{
    assert(dev != NULL);
    assert(metadata_rx_has_epoch_samples(metadata));
    assert(layout == BLADERF_RX_X1 || layout == BLADERF_RX_X2);
    sync_host_data_events++;
    sync_host_data_order = ++sync_event_order;
}

static void note_rx_overrun(struct bladerf *dev, uint32_t source_flags)
{
    assert(dev != NULL);
    rx_overrun_events++;
    last_rx_overrun_source_flags = source_flags;
    sync_overrun_order = ++sync_event_order;
}

static void note_async_withheld(struct bladerf *dev, uint32_t reason)
{
    assert(dev != NULL);
    async_withheld_events++;
    async_withheld_reason = reason;
    async_fault_withheld_order = ++async_fault_order;
}

static void note_sync_withheld(struct bladerf *dev, uint32_t reason)
{
    assert(dev != NULL);
    sync_withheld_events++;
    sync_withheld_order = ++sync_event_order;
    sync_withheld_reason = reason;
    atomic_store(&sync_withheld_observed, true);
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
    async_fault_overrun_order = ++async_fault_order;
}

static void note_worker_overrun(struct bladerf *dev, uint32_t source_flags)
{
    assert(dev != NULL);
    rx_worker_overrun_events++;
    last_rx_worker_overrun_source_flags = source_flags;
}

static unsigned int async_rx_callbacks;
static unsigned int async_rx_event_wakeups;
static bool allow_async_rx_buffer = true;
static bool check_async_rx_channel_mask;
static bool async_rx_channel_mask_valid = true;
static uint8_t async_rx_channel_mask;
static bladerf_channel async_rx_transition_channel;
static void *async_rejected_replacement;

static void *replace_rejected_async_buffer(void *user_data, void *buffer)
{
    assert(user_data != NULL && buffer != NULL);
    async_fault_rejected_order = ++async_fault_order;
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
    return allow_async_rx_buffer &&
        (!check_async_rx_channel_mask ||
         bladerf2_rx_layout_matches_channel_mask(
             layout, async_rx_transition_channel,
             async_rx_channel_mask_valid, async_rx_channel_mask));
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
        async_fault_callback_order = ++async_fault_order;
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
    .rx_sync_channel_selection_valid = validate_sync_channel_selection,
    .rx_async_stream_overrun = note_async_overrun,
    .rx_worker_stream_overrun = note_worker_overrun,
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
    rx_worker_overrun_events = 0;
    last_rx_worker_overrun_source_flags = 0;
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
    assert(MUTEX_INIT(&f->sync.rx_epoch_generation_lock) == 0);
    f->sync.rx_epoch_generation = 0;
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
    assert(pthread_mutex_destroy(&f->sync.rx_epoch_generation_lock) == 0);
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

static void test_sync_channel_selection_fail_closed(void)
{
    struct fixture f;
    struct bladerf_metadata meta = {0};
    int16_t out[64];

    fixture_init(&f);
    for (size_t i = 0; i < sizeof(out) / sizeof(out[0]); ++i) {
        out[i] = 0x5a5a;
    }
    write_msg(f.buffers[0], 1000, 7, 2222);
    allow_sync_channel_selection = false;
    meta.flags = BLADERF_META_FLAG_RX_NOW;
    assert(sync_rx(&f.sync, out, 32, &meta, 0) == BLADERF_ERR_WOULD_BLOCK);
    assert(meta.actual_count == 0);
    for (size_t i = 0; i < sizeof(out) / sizeof(out[0]); ++i) {
        assert(out[i] == 0x5a5a);
    }
    assert(sync_withheld_events == 1);
    assert(sync_withheld_reason == BLADERF_RF_WITHHELD_RX_CHANNEL_SELECTION);
    assert(sync_withheld_timestamp_valid[0]);
    assert(sync_withheld_epochs[0] == 7);
    assert(sync_withheld_timestamps[0] == 1000);
    assert(sync_host_data_events == 0);
    allow_sync_channel_selection = true;
    fixture_destroy(&f);
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
    assert(COND_INIT(&board_data->rx_async_epoch_cond) == 0);
    board_data->rf_transition_epoch_contract_enabled = true;
    board_data->rf_transition_first_host_data_required = true;
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
    assert(board_data->rf_transition_first_host_data_failure ==
           BLADERF_ERR_UNSUPPORTED);

    MUTEX_LOCK(&board_data->rx_async_epoch_lock);
    board_data->rx_format_unsupported_reported = false;
    board_data->rf_transition_first_host_data_required = false;
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
    COND_DESTROY(&board_data->rx_async_epoch_cond);
    MUTEX_DESTROY(&board_data->rx_async_epoch_lock);
    MUTEX_DESTROY(&dev.lock);
    free(board_data);
}

static void test_worker_overrun_event_history_is_lock_safe(void)
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
    board_data->rf_transition_epoch_certified = true;
    board_data->rf_transition_certified_epoch_id = 9;
    board_data->rf_transition_epoch_id = 10;

    /* A setter may hold dev->lock while the sync worker reports ring loss.
     * The event writer must append without taking that lock. */
    MUTEX_LOCK(&dev.lock);
    bladerf2_rx_worker_stream_overrun(
        &dev, BLADERF_RF_STREAM_STATUS_SYNC_RX_QUEUE |
                  BLADERF_RF_STREAM_STATUS_SYNC_RX_RING_FULL);
    MUTEX_UNLOCK(&dev.lock);

    assert(board_data->rf_transition_event_count == 1);
    uint32_t latest = (board_data->rf_transition_event_head +
                       BLADERF2_RF_EVENT_HISTORY_SIZE - 1) %
                      BLADERF2_RF_EVENT_HISTORY_SIZE;
    const struct bladerf_rf_event *event =
        &board_data->rf_transition_events[latest];
    assert(event->event_type == BLADERF_RF_EVT_RX_STREAM_OVERRUN);
    assert(event->epoch_id == 9);
    assert(event->fpga_state == BLADERF_RF_STATE_RX_DATA_VALID);
    assert(event->flags == (BLADERF_RF_STREAM_STATUS_OVERRUN |
                            BLADERF_RF_STREAM_STATUS_SYNC_RX_QUEUE |
                            BLADERF_RF_STREAM_STATUS_SYNC_RX_RING_FULL));
    assert(board_data->rx_async_data_withheld_active);

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

    /* Model the rearm performed after a certified packet. A later fault in
     * this same epoch must produce a fresh notification. */
    MUTEX_LOCK(&board_data->rx_async_epoch_lock);
    bladerf2_rx_data_rearm_notifications_locked(board_data);
    MUTEX_UNLOCK(&board_data->rx_async_epoch_lock);
    bladerf2_rx_data_withheld(
        &dev, BLADERF_RF_WITHHELD_EPOCH_OR_TIMESTAMP_MISMATCH);
    assert(board_data->rf_transition_event_count == 2);

    /* Explicit invalidation also rearms notification coalescing. */
    bladerf2_rx_data_withheld_reset(&dev);
    bladerf2_rx_data_withheld(
        &dev, BLADERF_RF_WITHHELD_EPOCH_UNCERTIFIED);
    assert(board_data->rf_transition_event_count == 3);

    board_data->rf_transition_epoch_contract_enabled = false;
    MUTEX_LOCK(&dev.lock);
    bladerf2_rx_data_withheld(
        &dev, BLADERF_RF_WITHHELD_SHORT_TRANSFER);
    MUTEX_UNLOCK(&dev.lock);
    assert(board_data->rf_transition_event_count == 4);
    event = &board_data->rf_transition_events[3];
    assert(event->event_type == BLADERF_RF_EVT_RX_DATA_WITHHELD);
    assert(event->flags == BLADERF_RF_WITHHELD_SHORT_TRANSFER);
    bladerf2_rx_data_withheld(
        &dev, BLADERF_RF_WITHHELD_USB_OVERFLOW);
    assert(board_data->rf_transition_event_count == 5);
    event = &board_data->rf_transition_events[4];
    assert(event->event_type == BLADERF_RF_EVT_RX_DATA_WITHHELD);
    assert(event->flags == BLADERF_RF_WITHHELD_USB_OVERFLOW);
    MUTEX_LOCK(&dev.lock);
    bladerf2_rx_async_stream_overrun(&dev);
    MUTEX_UNLOCK(&dev.lock);
    assert(board_data->rf_transition_event_count == 6);
    event = &board_data->rf_transition_events[5];
    assert(event->event_type == BLADERF_RF_EVT_RX_STREAM_OVERRUN);
    assert(event->flags == (BLADERF_RF_STREAM_STATUS_OVERRUN |
                            BLADERF_RF_STREAM_STATUS_ASYNC_USB));

    MUTEX_DESTROY(&board_data->rf_transition_event_lock);
    MUTEX_DESTROY(&board_data->rx_async_epoch_lock);
    MUTEX_DESTROY(&dev.lock);
    free(board_data);
}

static void test_host_data_event_uses_epoch_snapshot(void)
{
    struct bladerf2_board_data board_data = {0};
    struct bladerf_rf_event filler = {0};
    struct bladerf_metadata metadata = {0};
    const uint32_t capacity = BLADERF2_RF_EVENT_HISTORY_SIZE;

    assert(MUTEX_INIT(&board_data.rx_async_epoch_lock) == 0);
    assert(COND_INIT(&board_data.rx_async_epoch_cond) == 0);
    assert(MUTEX_INIT(&board_data.rf_transition_event_lock) == 0);
    board_data.rf_transition_epoch_contract_enabled = true;
    board_data.rf_transition_epoch_certified = true;
    board_data.rf_transition_current_channel = BLADERF_CHANNEL_RX(1);
    board_data.rx_channel_enable_mask_valid = true;
    board_data.rx_channel_enable_mask = 0x3;
    board_data.rf_transition_rx_x2_host_data_required = true;
    board_data.rf_transition_rx_x2_host_data_transaction_id = 123;
    board_data.rf_transition_certified_epoch_id = 5;
    board_data.rf_transition_first_valid_timestamp = 1000;
    board_data.rf_transition_certified_epoch_event.transaction_id = 77;
    board_data.rf_transition_certified_epoch_event.epoch_id = 5;
    board_data.rf_transition_certified_epoch_event.event_type =
        BLADERF_RF_EVT_RX_EPOCH_VALID;
    board_data.rf_transition_certified_epoch_event.flags =
        BLADERF_RF_EVENT_F_TRANSITION_CHANNEL_VALID |
        BLADERF_RF_EVENT_F_TRANSITION_RX2;
    board_data.rf_transition_certified_epoch_event.fpga_timestamp = 1000;
    board_data.rf_transition_certified_epoch_event.requested_rx_lo_hz =
        1835000000ULL;
    board_data.rf_transition_certified_epoch_event.readback_rx_lo_hz =
        1835000000ULL;

    /* Simulate a saturated ring whose retained entries no longer contain
     * RX_EPOCH_VALID. The durable snapshot must still produce the host IQ
     * event with its transition identity. */
    filler.event_type = BLADERF_RF_EVT_RX_STREAM_OVERRUN;
    for (uint32_t i = 0; i < capacity; ++i) {
        bladerf2_rf_event_append_locked(&board_data, &filler);
    }
    metadata.actual_count = 8;
    metadata.rx_epoch_id_valid = 1;
    metadata.rx_epoch_id = 5;
    metadata.timestamp = 1001;

    MUTEX_LOCK(&board_data.rx_async_epoch_lock);
    bladerf2_rx_data_note_first_packet_locked(&board_data, &metadata,
                                               BLADERF_RX_X2);
    MUTEX_UNLOCK(&board_data.rx_async_epoch_lock);
    assert(board_data.rf_transition_first_host_data_reported);
    assert(board_data.rf_transition_event_sequence == capacity + 1);
    uint32_t latest = (board_data.rf_transition_event_head + capacity - 1) %
                      capacity;
    assert(board_data.rf_transition_events[latest].event_type ==
           BLADERF_RF_EVT_RX_FIRST_VALID_HOST_DATA);
    assert(board_data.rf_transition_events[latest].transaction_id == 77);
    assert(board_data.rf_transition_events[latest].epoch_id == 5);
    assert(board_data.rf_transition_events[latest].fpga_timestamp == 1001);
    assert(board_data.rf_transition_first_host_data_event.transaction_id == 77);
    assert(board_data.rf_transition_first_host_data_event.event_type ==
           BLADERF_RF_EVT_RX_FIRST_VALID_HOST_DATA);
    assert(board_data.rf_transition_first_host_data_event.flags &
           BLADERF_RF_EVENT_F_RX_X2_LAYOUT);
    assert(board_data.rf_transition_first_host_data_event.flags &
           BLADERF_RF_EVENT_F_TRANSITION_RX2);

    /* When the request explicitly requires paired host data, an X1 packet
     * cannot satisfy or wake the transition waiter. */
    metadata.timestamp = 1002;
    MUTEX_LOCK(&board_data.rx_async_epoch_lock);
    bladerf2_rx_data_note_first_packet_locked(&board_data, &metadata,
                                               BLADERF_RX_X1);
    MUTEX_UNLOCK(&board_data.rx_async_epoch_lock);
    assert(board_data.rf_transition_first_host_data_reported);
    assert(board_data.rf_transition_event_sequence == capacity + 1);
    assert(board_data.rf_transition_first_host_data_event.fpga_timestamp ==
           1001);

    board_data.rf_transition_rx_x2_host_data_required = false;
    board_data.rx_async_data_withheld_active = true;
    board_data.rx_channel_enable_mask = 0x2;
    metadata.timestamp = 1003;
    MUTEX_LOCK(&board_data.rx_async_epoch_lock);
    bladerf2_rx_data_note_first_packet_locked(&board_data, &metadata,
                                               BLADERF_RX_X1);
    MUTEX_UNLOCK(&board_data.rx_async_epoch_lock);
    latest = (board_data.rf_transition_event_head + capacity - 1) % capacity;
    assert(board_data.rf_transition_events[latest].event_type ==
           BLADERF_RF_EVT_RX_DATA_RESUMED);
    assert(board_data.rf_transition_events[latest].transaction_id == 77);
    assert(!(board_data.rf_transition_events[latest].flags &
             BLADERF_RF_EVENT_F_RX_X2_LAYOUT));
    assert(board_data.rf_transition_events[latest].flags &
           BLADERF_RF_EVENT_F_TRANSITION_RX2);

    /* An RX_X1 packet with only RX1 enabled cannot stand in for a transition
     * requested through RX2. Record the mismatch and do not report IQ valid. */
    board_data.rx_channel_enable_mask = 0x1;
    board_data.rf_transition_first_host_data_required = true;
    board_data.rf_transition_first_host_data_reported = false;
    metadata.timestamp = 1004;
    MUTEX_LOCK(&board_data.rx_async_epoch_lock);
    bladerf2_rx_data_note_first_packet_locked(&board_data, &metadata,
                                               BLADERF_RX_X1);
    MUTEX_UNLOCK(&board_data.rx_async_epoch_lock);
    latest = (board_data.rf_transition_event_head + capacity - 1) % capacity;
    assert(board_data.rf_transition_events[latest].event_type ==
           BLADERF_RF_EVT_RX_DATA_WITHHELD);
    assert(board_data.rf_transition_events[latest].flags ==
           (BLADERF_RF_WITHHELD_RX_CHANNEL_SELECTION |
            BLADERF_RF_EVENT_F_FPGA_TIMESTAMP_VALID |
            BLADERF_RF_EVENT_F_TRANSITION_RX2 |
            BLADERF_RF_EVENT_F_TRANSITION_CHANNEL_VALID));
    assert(board_data.rf_transition_first_host_data_failure ==
           BLADERF_ERR_UNSUPPORTED);

    COND_DESTROY(&board_data.rx_async_epoch_cond);
    MUTEX_DESTROY(&board_data.rf_transition_event_lock);
    MUTEX_DESTROY(&board_data.rx_async_epoch_lock);
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

static void test_rx_x2_layout_rejection_event(void)
{
    struct bladerf2_board_data board_data = {0};
    struct bladerf dev = {0};

    assert(MUTEX_INIT(&board_data.rx_async_epoch_lock) == 0);
    assert(COND_INIT(&board_data.rx_async_epoch_cond) == 0);
    assert(MUTEX_INIT(&board_data.rf_transition_event_lock) == 0);
    dev.board_data = &board_data;
    board_data.rf_transition_rx_x2_host_data_required = true;
    board_data.rf_transition_rx_x2_host_data_transaction_id = 123;
    board_data.rf_transition_epoch_certified = true;
    board_data.rf_transition_first_host_data_required = true;
    board_data.rf_transition_certified_epoch_id = 9;
    board_data.rf_transition_certified_epoch_event.transaction_id = 123;

    bladerf2_rx_layout_unsupported(&dev, BLADERF_RX_X1, true);
    assert(board_data.rf_transition_event_count == 1);
    const uint32_t latest = (board_data.rf_transition_event_head +
        BLADERF2_RF_EVENT_HISTORY_SIZE - 1) %
        BLADERF2_RF_EVENT_HISTORY_SIZE;
    const struct bladerf_rf_event *event =
        &board_data.rf_transition_events[latest];
    assert(event->event_type == BLADERF_RF_EVT_RX_LAYOUT_UNSUPPORTED);
    assert(event->transaction_id == 123);
    assert(event->epoch_id == 9);
    assert(event->flags == BLADERF_RX_X1);
    assert(event->error_code == BLADERF_ERR_UNSUPPORTED);
    assert(board_data.rf_transition_first_host_data_failure ==
           BLADERF_ERR_UNSUPPORTED);

    COND_DESTROY(&board_data.rx_async_epoch_cond);
    MUTEX_DESTROY(&board_data.rf_transition_event_lock);
    MUTEX_DESTROY(&board_data.rx_async_epoch_lock);
}

static void test_sync_worker_overrun_published_before_sync_read(void)
{
    struct fixture f;
    uint32_t buffer_seq[2] = {0, 1};
    bool buffer_dropped[2] = {false, false};

    fixture_init(&f);
    f.sync.buf_mgmt.buffer_seq = buffer_seq;
    f.sync.buf_mgmt.buffer_dropped = buffer_dropped;
    f.sync.buf_mgmt.expected_seq = 0;
    f.sync.buf_mgmt.next_seq = 2;
    f.sync.buf_mgmt.prod_i = 1;
    f.states[0] = SYNC_BUFFER_IN_FLIGHT;
    f.states[1] = SYNC_BUFFER_FULL;

    /* Reusing the rejected completion has no free ring slot. The worker must
     * publish an event now; waiting for a later sync_rx() could hide a fault
     * from an independent wrapper event poller indefinitely. */
    assert(sync_worker_rx_buffer_rejected(&f.sync, f.buffers[0]) ==
           f.buffers[0]);
    assert(rx_worker_overrun_events == 1);
    assert(last_rx_worker_overrun_source_flags ==
           (BLADERF_RF_STREAM_STATUS_SYNC_RX_QUEUE |
            BLADERF_RF_STREAM_STATUS_SYNC_RX_RING_FULL));
    assert(f.sync.buf_mgmt.overrun_pending);
    assert(f.sync.buf_mgmt.overrun_event_published);

    fixture_destroy(&f);
}

struct blocked_sync_read {
    struct fixture *fixture;
    int16_t samples[2 * MSG_SAMPLES];
    struct bladerf_metadata metadata;
    atomic_bool done;
    int status;
};

static void *blocked_sync_read_thread(void *arg)
{
    struct blocked_sync_read *read = arg;
    memset(&read->metadata, 0, sizeof(read->metadata));
    read->metadata.flags = BLADERF_META_FLAG_RX_NOW;
    read->status = sync_rx(&read->fixture->sync, read->samples, 100,
                           &read->metadata, 300);
    atomic_store(&read->done, true);
    return NULL;
}

static void test_meta_withheld_event_precedes_sync_read_timeout(void)
{
    struct fixture f;
    struct blocked_sync_read read = {0};
    pthread_t reader;

    fixture_init(&f);
    read.fixture = &f;
    write_msg(f.buffers[0], 1000, 7, 2101);
    write_msg(f.buffers[0] + MSG_BYTES, 1000 + MSG_SAMPLES, 7, 2201);
    write_msg(f.buffers[1], 1000 + 2 * MSG_SAMPLES, 7, 2301);
    write_msg(f.buffers[1] + MSG_BYTES, 1000 + 3 * MSG_SAMPLES, 7, 2401);
    assert(sync_rx_epoch_set_min_timestamp(&f.sync, 0, 8) == 0);
    atomic_store(&sync_withheld_observed, false);
    atomic_store(&read.done, false);
    assert(pthread_create(&reader, NULL, blocked_sync_read_thread, &read) == 0);

    /* The parser rejects the first old-epoch packet, then the read waits for
     * new-epoch data. The wrapper's native-event poller must see the reason
     * while that same sync_rx() call is still blocked, well before timeout. */
    struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000};
    for (unsigned int i = 0; i < 100 &&
         !atomic_load(&sync_withheld_observed); ++i) {
        nanosleep(&pause, NULL);
    }
    const bool event_before_deadline =
        atomic_load(&sync_withheld_observed) && !atomic_load(&read.done);
    assert(pthread_join(reader, NULL) == 0);

    assert(event_before_deadline);
    assert(read.status == BLADERF_ERR_TIMEOUT);
    assert(read.metadata.actual_count == 0);
    assert(sync_withheld_reasons[0] ==
           BLADERF_RF_WITHHELD_EPOCH_OR_TIMESTAMP_MISMATCH);
    assert(sync_withheld_events == 2);
    assert(sync_withheld_reasons[1] == BLADERF_RF_WITHHELD_SYNC_TIMEOUT);

    fixture_destroy(&f);
}

struct deadline_fence_call {
    struct bladerf_sync *sync;
    uint64_t deadline_ns;
    atomic_bool entered;
    int status;
};

static void *set_deadline_fence_thread(void *arg)
{
    struct deadline_fence_call *call = arg;
    atomic_store(&call->entered, true);
    call->status = sync_rx_epoch_set_min_timestamp_before_deadline(
        call->sync, 2000, 8, call->deadline_ns);
    return NULL;
}

struct epoch_commit_test_context {
    unsigned int calls;
    int status;
    struct bladerf_sync *sync;
    bool observed_sync_fenced;
};

static int test_epoch_admission_prepare(void *context, uint64_t deadline_ns)
{
    struct epoch_commit_test_context *test = context;
    assert(test != NULL);
    (void)deadline_ns;
    test->calls++;
    if (test->sync != NULL) {
        test->observed_sync_fenced =
            test->sync->meta.rx_epoch_data_invalidated;
    }
    return test->status;
}

static void test_epoch_admission_finish(void *context)
{
    assert(context != NULL);
}

struct admission_lock_probe {
    pthread_mutex_t *lock;
    atomic_bool started;
    atomic_bool acquired;
};

static void *probe_admission_lock(void *arg)
{
    struct admission_lock_probe *probe = arg;
    atomic_store(&probe->started, true);
    assert(pthread_mutex_lock(probe->lock) == 0);
    atomic_store(&probe->acquired, true);
    assert(pthread_mutex_unlock(probe->lock) == 0);
    return NULL;
}

static void test_async_epoch_certificate_commit(void)
{
    struct bladerf2_board_data board_data = {0};
    struct bladerf_rf_event event = {0};
    struct admission_lock_probe probe = {0};
    bool admission_lock_held = false;
    pthread_t thread;
    struct timespec pause = {.tv_sec = 0, .tv_nsec = 10000000};

    assert(MUTEX_INIT(&board_data.rx_async_epoch_lock) == 0);
    board_data.rf_transition_epoch_contract_enabled = true;
    event.event_type = BLADERF_RF_EVT_RX_EPOCH_VALID;
    event.fpga_state = BLADERF_RF_STATE_RX_DATA_VALID;
    event.transaction_id = 91;
    event.epoch_id = 23;
    event.fpga_timestamp = 987654;

    assert(bladerf2_rx_epoch_admission_prepare(
               &board_data, &event, 0, &admission_lock_held) ==
           BLADERF_ERR_TIMEOUT);
    assert(!admission_lock_held);
    assert(!board_data.rf_transition_epoch_certified);

    assert(bladerf2_rx_epoch_admission_prepare(
               &board_data, &event, UINT64_MAX,
               &admission_lock_held) == 0);
    assert(admission_lock_held);
    assert(board_data.rf_transition_epoch_certified);
    assert(board_data.rf_transition_certified_epoch_id == event.epoch_id);
    assert(board_data.rf_transition_first_valid_timestamp ==
           event.fpga_timestamp);
    assert(board_data.rf_transition_certified_epoch_event.transaction_id ==
           event.transaction_id);

    probe.lock = &board_data.rx_async_epoch_lock;
    atomic_init(&probe.started, false);
    atomic_init(&probe.acquired, false);
    assert(pthread_create(&thread, NULL, probe_admission_lock, &probe) == 0);
    while (!atomic_load(&probe.started)) {
        nanosleep(&(struct timespec){.tv_sec = 0, .tv_nsec = 1000000}, NULL);
    }
    nanosleep(&pause, NULL);
    assert(!atomic_load(&probe.acquired));
    bladerf2_rx_epoch_admission_finish(&board_data, &admission_lock_held);
    assert(!admission_lock_held);
    assert(pthread_join(thread, NULL) == 0);
    assert(atomic_load(&probe.acquired));
    assert(pthread_mutex_destroy(&board_data.rx_async_epoch_lock) == 0);
}

static void test_expired_transition_deadline_keeps_sync_rx_fenced(void)
{
    struct fixture f;
    struct bladerf_sync async_only_sync = {0};
    struct bladerf_metadata metadata = {0};
    struct epoch_commit_test_context commit = {0};
    int16_t samples[2 * MSG_SAMPLES] = {0};
    pthread_t fence_thread;
    struct deadline_fence_call call = {0};
    struct timespec now, hold = {.tv_sec = 0, .tv_nsec = 100000000};

    fixture_init(&f);
    assert(sync_rx_epoch_expect_id(&f.sync, 8) == 0);
    write_msg(f.buffers[0], 2000, 8, 1666);

    /* Async-only configurations have no sync parser to lock, but the
     * transition deadline still applies before their epoch-valid event. */
    assert(sync_rx_epoch_set_min_timestamp_before_deadline(
               &async_only_sync, 2000, 8, 0) == BLADERF_ERR_TIMEOUT);
    assert(sync_rx_epoch_activate_with_admission_before_deadline(
               &async_only_sync, 0, test_epoch_admission_prepare,
               test_epoch_admission_finish, &commit) == BLADERF_ERR_TIMEOUT);
    assert(commit.calls == 0);
    assert(sync_rx_epoch_activate_with_admission_before_deadline(
               &async_only_sync, UINT64_MAX, test_epoch_admission_prepare,
               test_epoch_admission_finish, &commit) == 0);
    assert(commit.calls == 1);

    /* A deadline already in the past must not clear the parser's invalid
     * latch, even when the queued packet has the expected epoch and sample
     * timestamp. */
    assert(sync_rx_epoch_set_min_timestamp_before_deadline(
               &f.sync, 2000, 8, 0) == BLADERF_ERR_TIMEOUT);
    assert(f.sync.meta.rx_epoch_data_invalidated);
    assert(f.sync.meta.rx_epoch_expected_id == 8);
    assert(f.sync.meta.rx_epoch_min_timestamp == 0);

    metadata.flags = BLADERF_META_FLAG_RX_NOW;
    assert(sync_rx(&f.sync, samples, MSG_SAMPLES, &metadata, 1) ==
           BLADERF_ERR_WOULD_BLOCK);
    assert(metadata.actual_count == 0);
    assert(f.sync.meta.rx_epoch_data_invalidated);

    /* Staging the FPGA boundary must not itself release samples. If event
     * publication consumes the deadline, the later activation fails closed. */
    assert(sync_rx_epoch_stage_min_timestamp_before_deadline(
               &f.sync, 2000, 8, UINT64_MAX) == 0);
    assert(f.sync.meta.rx_epoch_min_timestamp == 2000);
    assert(f.sync.meta.rx_epoch_data_invalidated);
    assert(sync_rx_epoch_activate_before_deadline(&f.sync, 0) ==
           BLADERF_ERR_TIMEOUT);
    assert(f.sync.meta.rx_epoch_data_invalidated);
    commit.status = BLADERF_ERR_UNEXPECTED;
    assert(sync_rx_epoch_activate_with_admission_before_deadline(
               &f.sync, UINT64_MAX, test_epoch_admission_prepare,
               test_epoch_admission_finish, &commit) == BLADERF_ERR_UNEXPECTED);
    assert(f.sync.meta.rx_epoch_data_invalidated);
    commit.status = 0;

    /* Also force the actual lock-wait race: the caller's deadline is live
     * when the thread starts, but expires while it is blocked on sync->lock.
     * The atomic deadline check must reject the update without admitting IQ. */
    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    call.sync = &f.sync;
    call.deadline_ns = (uint64_t)now.tv_sec * 1000000000ULL +
                       (uint64_t)now.tv_nsec + 50000000ULL;
    atomic_store(&call.entered, false);
    MUTEX_LOCK(&f.sync.lock);
    assert(pthread_create(&fence_thread, NULL,
                          set_deadline_fence_thread, &call) == 0);
    while (!atomic_load(&call.entered)) {
        nanosleep(&(struct timespec){.tv_sec = 0, .tv_nsec = 1000000}, NULL);
    }
    nanosleep(&hold, NULL);
    MUTEX_UNLOCK(&f.sync.lock);
    assert(pthread_join(fence_thread, NULL) == 0);
    assert(call.status == BLADERF_ERR_TIMEOUT);
    assert(f.sync.meta.rx_epoch_data_invalidated);

    /* A fresh explicit fence still reopens admission through the legacy
     * wrapper, which stages and activates with an unbounded deadline. */
    assert(sync_rx_epoch_set_min_timestamp(&f.sync, 2000, 8) == 0);
    assert(!f.sync.meta.rx_epoch_data_invalidated);

    /* The transaction commit callback runs while the parser remains fenced;
     * only after it succeeds is the sync latch cleared. */
    assert(sync_rx_epoch_invalidate(&f.sync) == 0);
    assert(f.sync.meta.rx_epoch_data_invalidated);
    commit.sync = &f.sync;
    assert(sync_rx_epoch_activate_with_admission_before_deadline(
               &f.sync, UINT64_MAX, test_epoch_admission_prepare,
               test_epoch_admission_finish, &commit) == 0);
    assert(commit.calls == 3);
    assert(commit.observed_sync_fenced);
    assert(!f.sync.meta.rx_epoch_data_invalidated);
    fixture_destroy(&f);
}

int main(void)
{
    test_rx_channel_mask_runtime_policy();
    test_sync_channel_selection_fail_closed();
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
    test_worker_overrun_event_history_is_lock_safe();
    test_sync_worker_overrun_published_before_sync_read();
    test_meta_withheld_event_precedes_sync_read_timeout();
    test_async_epoch_certificate_commit();
    test_expired_transition_deadline_keeps_sync_rx_fenced();
    test_async_data_withheld_event();
    test_host_data_event_uses_epoch_snapshot();
    test_rx_x2_layout_rejection_event();
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

    /* The very first event-driven transition must enable epoch filtering
     * even when sync streaming started before any certificate existed. */
    fixture_init(&f);
    f.sync.meta.rx_epoch_boundary_enabled = false;
    f.sync.meta.rx_epoch_id_filter_enabled = false;
    f.sync.meta.rx_epoch_data_invalidated = false;
    assert(sync_rx_epoch_invalidate(&f.sync) == 0);
    assert(f.sync.meta.rx_epoch_boundary_enabled);
    assert(f.sync.meta.rx_epoch_id_filter_enabled);
    assert(f.sync.meta.rx_epoch_data_invalidated);
    assert(f.states[0] == SYNC_BUFFER_EMPTY);
    assert(f.states[1] == SYNC_BUFFER_EMPTY);
    fixture_destroy(&f);

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
    /* Invalidation discards the old partial message and resets the parser;
     * no pre-retune IQ may survive until the next certified epoch. */
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
    assert(sync_withheld_events == 1);
    assert(sync_withheld_reasons[0] == BLADERF_RF_WITHHELD_SYNC_TIMEOUT);
    assert(!sync_withheld_timestamp_valid[0]);

    /* A later explicit epoch must recover this same parser after the
     * timed-out read and discard all queued data from the previous epoch.
     * Model the first USB completion after reactivation as stale then valid
     * META messages in one sync ring buffer. */
    assert(sync_rx_epoch_invalidate(&f.sync) == 0);
    assert(sync_rx_epoch_expect_id(&f.sync, 9) == 0);
    assert(sync_rx_epoch_set_min_timestamp(&f.sync, 9000, 9) == 0);
    assert(f.states[0] == SYNC_BUFFER_EMPTY);
    assert(f.states[1] == SYNC_BUFFER_EMPTY);
    write_msg(f.buffers[0], 5000, 8, 1211);
    write_msg(f.buffers[0] + MSG_BYTES, 9000, 9, 1311);
    f.states[0] = SYNC_BUFFER_FULL;
    f.lengths[0] = BYTES_PER_BUFFER;
    receive(&f, out, 100, &meta);
    assert(meta.timestamp == 9000);
    assert(meta.rx_epoch_id_valid && meta.rx_epoch_id == 9);
    assert(meta.actual_count == 100);
    assert_marker(out, 100, 1311, 0);
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

    /* A stale RX_X2 META message contains a paired RX1/RX2 payload. The
     * epoch fence must discard the whole pair and start both interleaved
     * channels at the first paired sample of the certified epoch. */
    fixture_init(&f);
    f.sync.stream_config.layout = BLADERF_RX_X2;
    f.sync.meta.samples_per_ts = 2;
    assert(sync_rx_epoch_expect_id(&f.sync, 8) == 0);
    assert(sync_rx_epoch_set_min_timestamp(&f.sync, 3000, 8) == 0);
    write_msg(f.buffers[0], 2000, 7, 1777);
    write_msg(f.buffers[0] + MSG_BYTES, 3000, 8, 1888);
    memset(&meta, 0, sizeof(meta));
    meta.flags = BLADERF_META_FLAG_RX_NOW;
    assert(sync_rx(&f.sync, out, MSG_SAMPLES, &meta, 0) == 0);
    assert(meta.actual_count == MSG_SAMPLES);
    assert(meta.timestamp == 3000);
    assert(meta.rx_epoch_id_valid && meta.rx_epoch_id == 8);
    assert_marker(out, MSG_SAMPLES, 1888, 0);
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
    f.sync.buf_mgmt.overrun_source_flags =
        BLADERF_RF_STREAM_STATUS_SYNC_RX_QUEUE |
        BLADERF_RF_STREAM_STATUS_SYNC_RX_RING_FULL;
    assert(sync_rx(&f.sync, out, 8, NULL, 0) == BLADERF_ERR_WOULD_BLOCK);
    assert(rx_overrun_events == 2);
    assert(last_rx_overrun_source_flags ==
           (BLADERF_RF_STREAM_STATUS_SYNC_RX_QUEUE |
            BLADERF_RF_STREAM_STATUS_SYNC_RX_RING_FULL));
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
    async_fault_order = 0;
    async_fault_withheld_order = 0;
    async_fault_overrun_order = 0;
    async_fault_rejected_order = 0;
    async_fault_callback_order = 0;
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
    assert(async_fault_withheld_order < async_fault_overrun_order);
    assert(async_fault_overrun_order < async_fault_rejected_order);
    assert(async_fault_rejected_order < async_fault_callback_order);
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
    allow_async_rx_buffer = true;
    check_async_rx_channel_mask = true;
    async_rx_channel_mask_valid = true;
    async_rx_channel_mask = 0x1;
    async_rx_transition_channel = BLADERF_CHANNEL_RX(0);
    assert(async_rx_process_buffer(&async_stream, &async_meta, async_samples,
                                   sizeof(async_samples)) ==
           async_replacement);
    assert(async_rx_callbacks == 0);
    assert(async_rx_event_wakeups == 1);
    assert(async_rx_process_buffer(&async_stream, &async_meta, async_samples,
                                   sizeof(async_samples)) ==
           async_replacement);
    assert(async_rx_event_wakeups == 1);
    async_rx_channel_mask = 0x3;
    assert(async_rx_process_buffer(&async_stream, &async_meta, async_samples,
                                   sizeof(async_samples)) == async_samples);
    assert(async_rx_callbacks == 1);
    assert(async_rx_event_wakeups == 1);
    async_stream.layout = BLADERF_RX_X1;
    async_rx_transition_channel = BLADERF_CHANNEL_RX(1);
    async_rx_channel_mask = 0x1;
    assert(async_rx_process_buffer(&async_stream, &async_meta, async_samples,
                                   sizeof(async_samples)) ==
           async_replacement);
    assert(async_rx_callbacks == 1);
    assert(async_rx_event_wakeups == 2);
    async_rx_channel_mask = 0x2;
    assert(async_rx_process_buffer(&async_stream, &async_meta, async_samples,
                                   sizeof(async_samples)) == async_samples);
    assert(async_rx_callbacks == 2);
    assert(async_rx_event_wakeups == 2);
    check_async_rx_channel_mask = false;
    assert(async_rx_process_buffer(&async_stream, &async_meta, async_samples,
                                   sizeof(async_samples)) == async_samples);
    assert(async_rx_callbacks == 3);
    assert(async_rx_event_wakeups == 2);
    assert(rx_overrun_events == 2);
    allow_async_rx_buffer = false;
    async_stream.format = BLADERF_FORMAT_PACKET_META;
    assert(async_rx_process_buffer(&async_stream, &async_meta, async_samples,
                                   sizeof(async_samples)) ==
           async_replacement);
    assert(async_rx_callbacks == 3);
    assert(async_rx_event_wakeups == 3);
    allow_async_rx_buffer = true;
    assert(async_rx_process_buffer(&async_stream, &async_meta, async_samples,
                                   sizeof(async_samples)) == async_samples);
    assert(async_rx_callbacks == 4);
    assert(async_rx_event_wakeups == 3);
    async_stream.layout = BLADERF_TX_X2;
    async_notify_rx_overrun(&async_stream);
    async_notify_rx_overrun(NULL);
    assert(rx_overrun_events == 2);
    fixture_destroy(&f);

    return 0;
}
