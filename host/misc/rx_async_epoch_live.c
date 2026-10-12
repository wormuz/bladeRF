/* Live xA4 check: async RX must withhold IQ across invalid/cross-epoch data. */
#include <libbladeRF.h>
#include "timestamp_order.h"

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <time.h>

#include "host_config.h"
#include "streaming/metadata.h"

#define LIVE_RX_META_MESSAGE_SIZE 8192u
#define LIVE_RX_META_MESSAGE_SAMPLES \
    (LIVE_RX_META_MESSAGE_SIZE / sizeof(int32_t))
#define LIVE_RX_CAPTURED_EVENTS 128u
#define LIVE_RX_X2_SAMPLES_PER_TIMESTAMP 2u

enum live_rx_active_mode {
    LIVE_RX_ACTIVE_BOTH,
    LIVE_RX_ACTIVE_RX1,
    LIVE_RX_ACTIVE_RX2,
};

struct live_stream {
    struct bladerf *dev;
    struct bladerf_stream *stream;
    bladerf_channel_layout stream_layout;
    void **buffers;
    atomic_uint valid_callbacks;
    atomic_uint event_only_callbacks;
    atomic_bool invalidation_call_active;
    atomic_uint valid_callbacks_during_invalidation;
    atomic_bool stop;
    atomic_int event_query_status;
    uint64_t event_cursor;
    atomic_uint events_drained_from_callback;
    atomic_uint captured_event_count;
    struct bladerf_rf_event captured_events[LIVE_RX_CAPTURED_EVENTS];
    atomic_uint data_withheld_events;
    atomic_uint metadata_mismatch_withheld_events;
    atomic_uint timestamp_withheld_events;
    atomic_uint stream_overrun_events;
    atomic_uint event_history_gaps;
    atomic_bool pause_event_poll;
    atomic_uint timestamp_discontinuities;
    atomic_uint rx1_nonzero_slots;
    atomic_uint rx2_nonzero_slots;
    enum live_rx_active_mode active_mode;
    bladerf_channel transition_channel;
    bool have_expected_timestamp;
    uint8_t timestamp_epoch_id;
    uint64_t expected_timestamp;
    int stream_status;
};

static uint64_t monotonic_ns(void)
{
    struct timespec ts = {0};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) +
           (uint64_t)ts.tv_nsec;
}

static void poll_rf_events(struct bladerf *dev, struct live_stream *live)
{
    struct bladerf_rf_event events[BLADERF_RF_EVENT_HISTORY_SIZE];
    uint32_t event_count = 0;
    uint64_t next_sequence = live->event_cursor;
    bool history_complete = false;
    int query_status = bladerf_rf_events_get_since(
        dev, live->event_cursor, events, BLADERF_RF_EVENT_HISTORY_SIZE,
        &event_count, &next_sequence, &history_complete);
    if (query_status != 0 && history_complete) {
        atomic_store(&live->event_query_status, query_status);
        return;
    }

    live->event_cursor = next_sequence;
    atomic_fetch_add(&live->events_drained_from_callback, event_count);
    unsigned int captured = atomic_load_explicit(
        &live->captured_event_count, memory_order_relaxed);
    for (uint32_t i = 0; i < event_count &&
         captured < LIVE_RX_CAPTURED_EVENTS; ++i) {
        live->captured_events[captured++] = events[i];
    }
    atomic_store_explicit(&live->captured_event_count, captured,
                          memory_order_release);
    if (!history_complete) {
        atomic_fetch_add(&live->event_history_gaps, 1);
    }
    for (uint32_t i = 0; i < event_count; ++i) {
        if (events[i].event_type == BLADERF_RF_EVT_RX_DATA_WITHHELD) {
            atomic_fetch_add(&live->data_withheld_events, 1);
            if (events[i].flags ==
                BLADERF_RF_WITHHELD_TIMESTAMP_DISCONTINUITY) {
                atomic_fetch_add(&live->timestamp_withheld_events, 1);
            }
            if ((events[i].flags &
                 ~BLADERF_RF_EVENT_F_FPGA_TIMESTAMP_VALID) ==
                BLADERF_RF_WITHHELD_EPOCH_OR_TIMESTAMP_MISMATCH) {
                atomic_fetch_add(&live->metadata_mismatch_withheld_events,
                                 1);
            }
        } else if (events[i].event_type == BLADERF_RF_EVT_RX_STREAM_OVERRUN) {
            atomic_fetch_add(&live->stream_overrun_events, 1);
        }
    }
}

static void *rx_callback(struct bladerf *dev, struct bladerf_stream *stream,
                         struct bladerf_metadata *metadata, void *samples,
                         size_t num_samples, void *user_data)
{
    struct live_stream *live = user_data;
    (void)stream;

    if (atomic_load(&live->stop)) {
        return BLADERF_STREAM_SHUTDOWN;
    }
    if (!atomic_load(&live->pause_event_poll)) {
        poll_rf_events(dev, live);
    }
    if (num_samples == 0) {
        if (samples != NULL) {
            return BLADERF_STREAM_SHUTDOWN;
        }
        unsigned int event_only_count = atomic_fetch_add(
            &live->event_only_callbacks, 1) + 1;
        if (event_only_count == 1) {
            fprintf(stderr, "first event-only callback host_ns=%llu\n",
                    (unsigned long long)monotonic_ns());
        }
        /* This wakeup consumed no caller buffer; recycle the withheld USB
         * transfer instead of handing the backend an unrelated buffer. */
        return BLADERF_STREAM_REUSE_BUFFER;
    }
    if (samples == NULL) {
        return BLADERF_STREAM_SHUTDOWN;
    }

    if (atomic_load(&live->invalidation_call_active)) {
        atomic_fetch_add(&live->valid_callbacks_during_invalidation, 1);
    }

    /* The xA4 USB3 META stream uses 8 KiB messages. RX_X2 interleaves two
     * channel samples per FPGA timestamp tick; RX_X1 has one. Validate
     * continuity across every message and callback. The expected timestamp
     * survives event-only callbacks; a different epoch establishes a fresh
     * baseline. */
    const uint8_t *bytes = samples;
    const size_t received_bytes = num_samples * sizeof(int32_t);
    const size_t message_samples =
        (LIVE_RX_META_MESSAGE_SIZE - METADATA_HEADER_SIZE) / sizeof(int32_t);
    const size_t samples_per_timestamp =
        live->active_mode == LIVE_RX_ACTIVE_BOTH ?
            LIVE_RX_X2_SAMPLES_PER_TIMESTAMP : 1u;
    uint8_t first_epoch_id = 0;
    if (received_bytes < LIVE_RX_META_MESSAGE_SIZE ||
        !metadata->rx_epoch_id_valid || metadata->actual_count != num_samples ||
        !metadata_get_rx_epoch_id(bytes, &first_epoch_id) ||
        metadata->rx_epoch_id != first_epoch_id ||
        metadata->timestamp != metadata_get_timestamp(bytes)) {
        atomic_store(&live->event_query_status, BLADERF_ERR_UNEXPECTED);
        fprintf(stderr, "async callback metadata mismatch: count=%u/%zu "
                "epoch_valid=%u epoch=%u first_epoch=%u timestamp=%llu "
                "first_timestamp=%llu\n", metadata->actual_count, num_samples,
                metadata->rx_epoch_id_valid, metadata->rx_epoch_id,
                first_epoch_id, (unsigned long long)metadata->timestamp,
                (unsigned long long)metadata_get_timestamp(bytes));
        return BLADERF_STREAM_SHUTDOWN;
    }
    for (size_t offset = 0; offset + LIVE_RX_META_MESSAGE_SIZE <= received_bytes;
         offset += LIVE_RX_META_MESSAGE_SIZE) {
        const uint8_t *header = bytes + offset;
        uint8_t epoch_id;
        if (!metadata_get_rx_epoch_id(header, &epoch_id)) {
            atomic_store(&live->event_query_status, BLADERF_ERR_UNEXPECTED);
            return BLADERF_STREAM_SHUTDOWN;
        }
        const uint64_t timestamp = metadata_get_timestamp(header);
        const int32_t *interleaved = (const int32_t *)(
            header + METADATA_HEADER_SIZE);
        const size_t complex_slots =
            (LIVE_RX_META_MESSAGE_SIZE - METADATA_HEADER_SIZE) /
            sizeof(*interleaved);
        if (live->active_mode == LIVE_RX_ACTIVE_BOTH) {
            for (size_t slot = 0; slot + 1 < complex_slots; slot += 2) {
                if (interleaved[slot] != 0) {
                    atomic_fetch_add(&live->rx1_nonzero_slots, 1);
                }
                if (interleaved[slot + 1] != 0) {
                    atomic_fetch_add(&live->rx2_nonzero_slots, 1);
                }
            }
        } else {
            atomic_uint *active_slots =
                live->active_mode == LIVE_RX_ACTIVE_RX1 ?
                    &live->rx1_nonzero_slots : &live->rx2_nonzero_slots;
            for (size_t slot = 0; slot < complex_slots; ++slot) {
                if (interleaved[slot] != 0) {
                    atomic_fetch_add(active_slots, 1);
                }
            }
        }
        if (epoch_id == 1 && offset < 4 * LIVE_RX_META_MESSAGE_SIZE) {
            fprintf(stderr, "initial META epoch=1 msg=%zu timestamp=%llu\n",
                    offset / LIVE_RX_META_MESSAGE_SIZE,
                    (unsigned long long)timestamp);
        }
        if (live->have_expected_timestamp &&
            epoch_id == live->timestamp_epoch_id &&
            timestamp != live->expected_timestamp) {
            atomic_fetch_add(&live->timestamp_discontinuities, 1);
            fprintf(stderr, "async META timestamp delta epoch=%u previous=%llu "
                    "expected_delta=%zu actual_delta=%llu message=%zu\n",
                    epoch_id,
                    (unsigned long long)(live->expected_timestamp -
                        message_samples / samples_per_timestamp),
                    message_samples / samples_per_timestamp,
                    (unsigned long long)(timestamp -
                        (live->expected_timestamp - message_samples /
                         samples_per_timestamp)),
                    offset / LIVE_RX_META_MESSAGE_SIZE);
        }
        live->timestamp_epoch_id = epoch_id;
        live->expected_timestamp = timestamp +
            message_samples / samples_per_timestamp;
        live->have_expected_timestamp = true;
    }
    atomic_fetch_add(&live->valid_callbacks, 1);
    return samples;
}

static void *run_stream(void *arg)
{
    struct live_stream *live = arg;
    live->stream_status = bladerf_stream(live->stream, live->stream_layout);
    return NULL;
}

static void stop_stream(struct live_stream *live, pthread_t thread)
{
    atomic_store(&live->stop, true);
    (void)bladerf_submit_stream_buffer(live->stream, BLADERF_STREAM_SHUTDOWN,
                                      1000);
    pthread_join(thread, NULL);
}

static bool wait_for_count(atomic_uint *count, unsigned int minimum,
                           unsigned int timeout_ms)
{
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    do {
        int64_t elapsed_ns;
        if (atomic_load(count) >= minimum) {
            return true;
        }
        sched_yield();
        clock_gettime(CLOCK_MONOTONIC, &now);
        elapsed_ns = (int64_t)(now.tv_sec - start.tv_sec) * 1000000000LL +
                     (int64_t)now.tv_nsec - (int64_t)start.tv_nsec;
        if (elapsed_ns >= (int64_t)timeout_ms * 1000000LL) {
            break;
        }
    } while (true);
    return atomic_load(count) >= minimum;
}

static int event_transition(struct bladerf *dev, uint64_t frequency_hz,
                            bladerf_channel channel,
                            struct bladerf_rf_event *result)
{
    const struct bladerf_rx_transition_request request = {
        .target_frequency_hz = frequency_hz,
        .required_events_mask = BLADERF_RF_REQUIRE_PLL_LOCKED |
                                BLADERF_RF_REQUIRE_ENSM_RX,
        .timeout_ms = 2000,
        .require_rx_data_valid = true,
        .epoch_settle_samples = 0,
    };
    uint32_t transaction_id = 0;
    static unsigned int transition_call_count;
    const unsigned int call_id = ++transition_call_count;
    if (call_id <= 2) {
        fprintf(stderr, "transition[%u] begin-call host_ns=%llu\n", call_id,
                (unsigned long long)monotonic_ns());
    }
    int status = bladerf_rx_transition_begin(
        dev, channel, &request, &transaction_id);
    if (call_id <= 2) {
        fprintf(stderr, "transition[%u] begin-return host_ns=%llu status=%d "
                "transaction=%u\n", call_id,
                (unsigned long long)monotonic_ns(), status, transaction_id);
    }
    if (status == 0) {
        status = bladerf_rx_transition_wait(dev, transaction_id, result, 2000);
    }
    if (call_id <= 2) {
        fprintf(stderr, "transition[%u] wait-return host_ns=%llu status=%d "
                "event=%u\n", call_id,
                (unsigned long long)monotonic_ns(), status,
                status == 0 ? result->event_type : 0);
    }
    if (status == 0 &&
        (result->event_type != BLADERF_RF_EVT_RX_EPOCH_VALID ||
         result->fpga_state != BLADERF_RF_STATE_RX_DATA_VALID)) {
        return BLADERF_ERR_UNEXPECTED;
    }
    return status;
}

static int require_first_host_data_event(
    struct bladerf *dev, const struct bladerf_rf_event *epoch_event)
{
    struct bladerf_rf_event events[BLADERF_RF_EVENT_HISTORY_SIZE];
    uint32_t count = 0;
    bool complete = false;
    int status = bladerf_rx_transition_get_events(
        dev, epoch_event->transaction_id, events,
        BLADERF_RF_EVENT_HISTORY_SIZE, &count, &complete);
    if (status != 0 || !complete) {
        fprintf(stderr, "first-host-data trace unavailable txn=%u status=%s "
                "complete=%u count=%u\n", epoch_event->transaction_id,
                bladerf_strerror(status), complete, count);
        return status != 0 ? status : BLADERF_ERR_UNEXPECTED;
    }

    for (uint32_t i = 0; i < count; ++i) {
        if (events[i].event_type == BLADERF_RF_EVT_RX_FIRST_VALID_HOST_DATA &&
            events[i].epoch_id == epoch_event->epoch_id &&
            timestamp_at_or_after(events[i].fpga_timestamp,
                                  epoch_event->fpga_timestamp)) {
            return 0;
        }
    }

    fprintf(stderr, "no RX_FIRST_VALID_HOST_DATA for async epoch=%u txn=%u "
            "after %u valid callbacks\n", epoch_event->epoch_id,
            epoch_event->transaction_id, count);
    return BLADERF_ERR_UNEXPECTED;
}

static void dump_captured_events(const struct live_stream *live)
{
    unsigned int count = atomic_load_explicit(&live->captured_event_count,
                                               memory_order_acquire);
    if (count > LIVE_RX_CAPTURED_EVENTS) {
        count = LIVE_RX_CAPTURED_EVENTS;
    }
    fprintf(stderr, "captured RF events=%u\n", count);
    for (unsigned int i = 0; i < count; ++i) {
        const struct bladerf_rf_event *event = &live->captured_events[i];
        fprintf(stderr, "  event[%u] host_ns=%llu type=%u txn=%u epoch=%u "
                "flags=0x%x error=%d rfic=0x%x fpga_ts=%llu\n", i,
                (unsigned long long)event->host_monotonic_ns,
                event->event_type, event->transaction_id, event->epoch_id,
                event->flags, event->error_code, event->rfic_status,
                (unsigned long long)event->fpga_timestamp);
    }
}

static void dump_event_history(struct live_stream *live)
{
    struct bladerf_rf_event events[BLADERF_RF_EVENT_HISTORY_SIZE];
    uint32_t count = 0;
    uint64_t next_sequence = live->event_cursor;
    bool complete = false;
    int status = bladerf_rf_events_get_since(
        live->dev, live->event_cursor, events, BLADERF_RF_EVENT_HISTORY_SIZE,
        &count, &next_sequence, &complete);
    fprintf(stderr, "RF history since callback cursor %llu: status=%d "
            "events=%u next=%llu complete=%u\n",
            (unsigned long long)live->event_cursor, status, count,
            (unsigned long long)next_sequence, complete);
    for (uint32_t i = 0; status == 0 && i < count; ++i) {
        fprintf(stderr, "  event[%u] host_ns=%llu type=%u txn=%u epoch=%u "
                "state=%u flags=0x%x error=%d rfic=0x%x fpga_ts=%llu\n",
                i, (unsigned long long)events[i].host_monotonic_ns,
                events[i].event_type, events[i].transaction_id,
                events[i].epoch_id, events[i].fpga_state, events[i].flags,
                events[i].error_code, events[i].rfic_status,
                (unsigned long long)events[i].fpga_timestamp);
    }
}

int main(void)
{
    struct live_stream live = {0};
    pthread_t stream_thread;
    struct bladerf_rf_event event = {0};
    unsigned int cross_band_cycles = 70;
    unsigned int async_buffer_samples = 8192;
    const char *cycles_env = getenv("BLADERF_ASYNC_EPOCH_CYCLES");
    const char *buffer_samples_env =
        getenv("BLADERF_ASYNC_RX_BUFFER_SAMPLES");
    const char *active_env = getenv("BLADERF_ASYNC_RX_ACTIVE");
    const char *metadata_fault =
        getenv("BLADERF_TEST_RX_EPOCH_METADATA_FAULT");
    if (metadata_fault != NULL && metadata_fault[0] != '\0' &&
        strcmp(metadata_fault, "stale_epoch") != 0 &&
        strcmp(metadata_fault, "pre_boundary") != 0) {
        fprintf(stderr, "BLADERF_TEST_RX_EPOCH_METADATA_FAULT must be "
                "stale_epoch or pre_boundary\n");
        return 2;
    }
    if (cycles_env != NULL && cycles_env[0] != '\0') {
        char *end = NULL;
        unsigned long requested = strtoul(cycles_env, &end, 10);
        if (end == cycles_env || *end != '\0' || requested < 70 ||
            requested > 4096) {
            fprintf(stderr, "BLADERF_ASYNC_EPOCH_CYCLES must be 70..4096\n");
            return 2;
        }
        cross_band_cycles = (unsigned int)requested;
    }
    if (buffer_samples_env != NULL && buffer_samples_env[0] != '\0') {
        char *end = NULL;
        unsigned long requested = strtoul(buffer_samples_env, &end, 10);
        if (end == buffer_samples_env || *end != '\0' ||
            requested < LIVE_RX_META_MESSAGE_SAMPLES ||
            requested > 131072 ||
            requested % LIVE_RX_META_MESSAGE_SAMPLES != 0) {
            fprintf(stderr, "BLADERF_ASYNC_RX_BUFFER_SAMPLES must be a "
                    "multiple of %zu in range %zu..131072\n",
                    LIVE_RX_META_MESSAGE_SAMPLES,
                    LIVE_RX_META_MESSAGE_SAMPLES);
            return 2;
        }
        async_buffer_samples = (unsigned int)requested;
    }
    live.active_mode = LIVE_RX_ACTIVE_BOTH;
    live.transition_channel = BLADERF_CHANNEL_RX(1);
    if (active_env != NULL && strcmp(active_env, "RX1") == 0) {
        live.active_mode = LIVE_RX_ACTIVE_RX1;
        live.transition_channel = BLADERF_CHANNEL_RX(0);
    } else if (active_env != NULL && strcmp(active_env, "RX2") == 0) {
        live.active_mode = LIVE_RX_ACTIVE_RX2;
        live.transition_channel = BLADERF_CHANNEL_RX(1);
    } else if (active_env != NULL && strcmp(active_env, "BOTH") != 0) {
        fprintf(stderr, "BLADERF_ASYNC_RX_ACTIVE must be RX1, RX2, or BOTH\n");
        return 2;
    }
    live.stream_layout = live.active_mode == LIVE_RX_ACTIVE_BOTH ?
        BLADERF_RX_X2 : BLADERF_RX_X1;
    int status = bladerf_open(&live.dev, NULL);
    if (status != 0) {
        fprintf(stderr, "bladerf_open: %s\n", bladerf_strerror(status));
        return 1;
    }

    bladerf_log_set_verbosity(BLADERF_LOG_LEVEL_WARNING);
    status = bladerf_set_sample_rate(live.dev, BLADERF_CHANNEL_RX(0),
                                     4000000, NULL);
    if (status == 0) {
        status = bladerf_set_sample_rate(live.dev, BLADERF_CHANNEL_RX(1),
                                         4000000, NULL);
    }
    if (status == 0) {
        status = bladerf_set_bandwidth(live.dev, BLADERF_CHANNEL_RX(0),
                                       5000000, NULL);
    }
    if (status == 0) {
        status = bladerf_set_bandwidth(live.dev, BLADERF_CHANNEL_RX(1),
                                       5000000, NULL);
    }
    if (status == 0) {
        status = bladerf_set_gain(live.dev, BLADERF_CHANNEL_RX(0), 30);
    }
    if (status == 0) {
        status = bladerf_set_gain(live.dev, BLADERF_CHANNEL_RX(1), 30);
    }
    if (status == 0) {
        status = bladerf_init_stream(&live.stream, live.dev, rx_callback,
                                     &live.buffers, 16,
                                     BLADERF_FORMAT_SC16_Q11_META,
                                     async_buffer_samples, 8, &live);
    }
    if (status == 0 && live.active_mode != LIVE_RX_ACTIVE_RX2) {
        status = bladerf_enable_module(live.dev, BLADERF_CHANNEL_RX(0), true);
    }
    if (status == 0 && live.active_mode != LIVE_RX_ACTIVE_RX1) {
        status = bladerf_enable_module(live.dev, BLADERF_CHANNEL_RX(1), true);
    }
    if (status != 0) {
        fprintf(stderr, "async RX setup: %s\n",
                bladerf_strerror(status));
        goto cleanup;
    }

    if (pthread_create(&stream_thread, NULL, run_stream, &live) != 0) {
        fprintf(stderr, "pthread_create failed\n");
        status = BLADERF_ERR_UNEXPECTED;
        goto cleanup;
    }
    fprintf(stderr, "async stream started: buffer_samples=%u "
            "buffer_bytes=%u host_ns=%llu\n", async_buffer_samples,
            async_buffer_samples * (unsigned int)sizeof(int32_t),
            (unsigned long long)monotonic_ns());
    uint32_t config_gpio = 0;
    status = bladerf_config_gpio_read(live.dev, &config_gpio);
    if (status != 0) {
        fprintf(stderr, "config GPIO read before transition: %s\n",
                bladerf_strerror(status));
        stop_stream(&live, stream_thread);
        goto cleanup;
    }
    fprintf(stderr, "pre-transition config GPIO=0x%08x timestamp=%u "
            "packet=%u host_ns=%llu\n", config_gpio,
            (config_gpio >> 16) & 1u, (config_gpio >> 19) & 1u,
            (unsigned long long)monotonic_ns());
    /* Let the backend queue its initial IN transfers before the transition.
     * This is test orchestration only; it prevents the transition API from
     * racing the backend's initial transfer submission. */
    const struct timespec backend_startup = { .tv_sec = 0,
                                               .tv_nsec = 100000000 };
    nanosleep(&backend_startup, NULL);
    /* Do not wait for a callback to prove the stream has started: with no
     * certified RX epoch the FPGA may have no payload to complete the first
     * USB transfer, and waiting for event-only can wait until that transfer
     * has already timed out. The transition competes for dev->lock with the
     * stream's format setup, then starts as soon as that setup is committed. */
    status = event_transition(live.dev, 1835000000ULL,
                              live.transition_channel, &event);
    if (status != 0) {
        fprintf(stderr, "async RX initial transition: %s\n",
                bladerf_strerror(status));
        stop_stream(&live, stream_thread);
        goto cleanup;
    }
    fprintf(stderr, "initial epoch certified: %u first_valid_timestamp=%llu "
            "event_type=%u state=%u\n", event.epoch_id,
            (unsigned long long)event.fpga_timestamp, event.event_type,
            event.fpga_state);
    if (!wait_for_count(&live.valid_callbacks, 2, 3000)) {
        struct bladerf_rf_event pending[BLADERF_RF_EVENT_HISTORY_SIZE];
        uint32_t pending_count = 0;
        uint64_t next_sequence = 0;
        bool history_complete = false;
        (void)bladerf_rf_events_get_since(
            live.dev, 0, pending, BLADERF_RF_EVENT_HISTORY_SIZE,
            &pending_count, &next_sequence, &history_complete);
        fprintf(stderr, "no certified async RX buffers reached callback "
                "(event_only=%u withheld=%u overrun=%u events=%u complete=%u)\n",
                atomic_load(&live.event_only_callbacks),
                atomic_load(&live.data_withheld_events),
                atomic_load(&live.stream_overrun_events), pending_count,
                history_complete);
        for (uint32_t i = 0; i < pending_count; ++i) {
            fprintf(stderr, "  event[%u] host_ns=%llu type=%u txn=%u "
                    "state=%u epoch=%u flags=0x%x error=%d rfic=0x%x "
                    "ts=%llu\n", i,
                    (unsigned long long)pending[i].host_monotonic_ns,
                    pending[i].event_type, pending[i].transaction_id,
                    pending[i].fpga_state, pending[i].epoch_id,
                    pending[i].flags, pending[i].error_code,
                    pending[i].rfic_status,
                    (unsigned long long)pending[i].fpga_timestamp);
        }
        dump_captured_events(&live);
        status = BLADERF_ERR_TIMEOUT;
        stop_stream(&live, stream_thread);
        goto cleanup;
    }
    fprintf(stderr, "initial valid callbacks: %u\n",
            atomic_load(&live.valid_callbacks));

#ifdef BLADERF_ENABLE_TEST_RX_EPOCH_METADATA_FAULT_INJECTION
    if (metadata_fault != NULL && metadata_fault[0] != '\0' &&
        !wait_for_count(&live.metadata_mismatch_withheld_events, 1, 3000)) {
        fprintf(stderr, "one-shot injected META fault was not withheld/reported\n");
        status = BLADERF_ERR_UNEXPECTED;
        stop_stream(&live, stream_thread);
        goto cleanup;
    }
#else
    if (metadata_fault != NULL && metadata_fault[0] != '\0') {
        fprintf(stderr, "metadata fault requested, but test injection is disabled\n");
        status = BLADERF_ERR_INVAL;
        stop_stream(&live, stream_thread);
        goto cleanup;
    }
#endif

    /* Same-value configuration still invalidates a prior RX certificate. */
    fprintf(stderr, "issuing invalidating gain setter\n");
    unsigned int invalidation_event_only_before =
        atomic_load(&live.event_only_callbacks);
    unsigned int invalidation_withheld_before =
        atomic_load(&live.data_withheld_events);
#ifdef BLADERF_ENABLE_TEST_RX_ABORT_FAULT_INJECTION
    /* Make the host-revoke/FPGA-ABORT window deterministic. Async callbacks
     * must switch to event-only before the delayed NIOS control command. */
    if (setenv("BLADERF_TEST_DELAY_RX_EPOCH_ABORT_MS", "250", 1) != 0) {
        status = BLADERF_ERR_UNEXPECTED;
        stop_stream(&live, stream_thread);
        goto cleanup;
    }
    atomic_store(&live.invalidation_call_active, true);
#endif
    status = bladerf_set_gain(live.dev, live.transition_channel, 30);
#ifdef BLADERF_ENABLE_TEST_RX_ABORT_FAULT_INJECTION
    atomic_store(&live.invalidation_call_active, false);
    unsetenv("BLADERF_TEST_DELAY_RX_EPOCH_ABORT_MS");
    if (atomic_load(&live.valid_callbacks_during_invalidation) != 0) {
        fprintf(stderr, "async IQ escaped during invalidation/ABORT window: "
                "%u callbacks\n",
                atomic_load(&live.valid_callbacks_during_invalidation));
        status = BLADERF_ERR_UNEXPECTED;
        stop_stream(&live, stream_thread);
        goto cleanup;
    }
    fprintf(stderr, "host revoke race: PASS valid callbacks during "
            "250ms ABORT delay=0\n");
#endif
    fprintf(stderr, "gain setter returned %d\n", status);
    if (status != 0) {
        fprintf(stderr, "gain invalidation failed: %s\n",
                bladerf_strerror(status));
        stop_stream(&live, stream_thread);
        goto cleanup;
    }
    unsigned int valid_before = atomic_load(&live.valid_callbacks);
    status = event_transition(live.dev, 1835400000ULL,
                              live.transition_channel, &event);
    if (status != 0 ||
        !wait_for_count(&live.event_only_callbacks,
                        invalidation_event_only_before + 1, 3000) ||
        !wait_for_count(&live.data_withheld_events,
                        invalidation_withheld_before + 1, 3000) ||
        !wait_for_count(&live.valid_callbacks, valid_before + 2, 3000)) {
        fprintf(stderr, "async RX did not withhold and recover around "
                "retune: %s (event_only=%u withheld=%u valid=%u)\n",
                bladerf_strerror(status),
                atomic_load(&live.event_only_callbacks),
                atomic_load(&live.data_withheld_events),
                atomic_load(&live.valid_callbacks));
        dump_captured_events(&live);
        status = status != 0 ? status : BLADERF_ERR_UNEXPECTED;
        stop_stream(&live, stream_thread);
        goto cleanup;
    }
    status = require_first_host_data_event(live.dev, &event);
    if (status != 0) {
        stop_stream(&live, stream_thread);
        goto cleanup;
    }
    fprintf(stderr, "async first-host-data event: PASS txn=%u epoch=%u\n",
            event.transaction_id, event.epoch_id);

    /* Overflow the bounded history while paired RX1+RX2 async traffic keeps
     * moving. Resuming callback polling must report the missing cursor range. */
    atomic_store(&live.pause_event_poll, true);
    unsigned int wrap_valid_target = atomic_load(&live.valid_callbacks);
    for (unsigned int i = 0; i < cross_band_cycles; ++i) {
        status = bladerf_set_gain(live.dev, live.transition_channel, 30);
        if (status != 0) {
            fprintf(stderr, "ring-wrap invalidation %u failed: %s\n", i,
                    bladerf_strerror(status));
            status = status != 0 ? status : BLADERF_ERR_UNEXPECTED;
            stop_stream(&live, stream_thread);
            goto cleanup;
        }
        /* Do not wait for an async callback to acknowledge the invalid
         * interval: no RF IQ may be available to complete the current USB
         * transfer. Start the causally required recovery transition as soon
         * as the invalidating setter returns. Alternate VCO bands on every
         * transition: 947.5 MHz and 1835 MHz. */
        const uint64_t target_frequency = (i % 2) == 0 ?
            947500000ULL : 1835000000ULL;
        const uint32_t previous_transaction_id = event.transaction_id;
        const uint8_t previous_epoch_id = event.epoch_id;
        status = event_transition(live.dev, target_frequency,
                                  live.transition_channel, &event);
        if (status == 0 &&
            (event.transaction_id != previous_transaction_id + 1 ||
             event.epoch_id == previous_epoch_id)) {
            fprintf(stderr, "transition identity did not advance at cycle %u "
                    "(transaction=%u epoch=%u, previous=%u/%u)\n",
                    i, event.transaction_id, event.epoch_id,
                    previous_transaction_id, previous_epoch_id);
            status = BLADERF_ERR_UNEXPECTED;
        }
        if (status != 0 ||
            !wait_for_count(&live.valid_callbacks, ++wrap_valid_target, 3000)) {
            fprintf(stderr, "ring-wrap recovery %u failed: %s "
                    "(valid=%u event_only=%u withheld=%u overrun=%u)\n", i,
                    bladerf_strerror(status),
                    atomic_load(&live.valid_callbacks),
                    atomic_load(&live.event_only_callbacks),
                    atomic_load(&live.data_withheld_events),
                    atomic_load(&live.stream_overrun_events));
            dump_event_history(&live);
            status = status != 0 ? status : BLADERF_ERR_UNEXPECTED;
            stop_stream(&live, stream_thread);
            goto cleanup;
        }
        const unsigned int progress_interval =
            cross_band_cycles > 100 ? 100 : 10;
        if ((i + 1) % progress_interval == 0) {
            fprintf(stderr,
                    "paired cross-band invalidation/recovery=%u/%u\n",
                    i + 1, cross_band_cycles);
        }
    }

    /* Trigger a callback-side read after wrap; it must surface an incomplete
     * cursor rather than silently treating the retained tail as complete. */
    atomic_store(&live.pause_event_poll, false);
    if (!wait_for_count(&live.event_history_gaps, 1, 3000)) {
        fprintf(stderr, "callback did not report overwritten RF history\n");
        status = BLADERF_ERR_UNEXPECTED;
        stop_stream(&live, stream_thread);
        goto cleanup;
    }

    if (atomic_load(&live.timestamp_discontinuities) != 0) {
        fprintf(stderr, "library exposed %u noncontiguous META buffers as IQ\n",
                atomic_load(&live.timestamp_discontinuities));
        status = BLADERF_ERR_UNEXPECTED;
        stop_stream(&live, stream_thread);
        goto cleanup;
    }
    if (atomic_load(&live.timestamp_withheld_events) !=
        atomic_load(&live.stream_overrun_events)) {
        fprintf(stderr, "timestamp discontinuity notification was incomplete "
                "(withheld=%u overrun=%u)\n",
                atomic_load(&live.timestamp_withheld_events),
                atomic_load(&live.stream_overrun_events));
        status = BLADERF_ERR_UNEXPECTED;
        stop_stream(&live, stream_thread);
        goto cleanup;
    }

    stop_stream(&live, stream_thread);
    if (live.stream_status != 0) {
        status = live.stream_status;
        fprintf(stderr, "async stream returned: %s\n",
                bladerf_strerror(status));
        goto cleanup;
    }

    if (atomic_load(&live.event_query_status) != 0 ||
        atomic_load(&live.events_drained_from_callback) == 0) {
        fprintf(stderr, "callback-side RF event history query failed\n");
        status = BLADERF_ERR_UNEXPECTED;
        goto cleanup;
    }
    if ((live.active_mode == LIVE_RX_ACTIVE_RX1 &&
         atomic_load(&live.rx1_nonzero_slots) == 0) ||
        (live.active_mode == LIVE_RX_ACTIVE_RX2 &&
         atomic_load(&live.rx2_nonzero_slots) == 0)) {
        fprintf(stderr, "active RX input produced no nonzero META slots "
                "(RX1=%u RX2=%u)\n",
                atomic_load(&live.rx1_nonzero_slots),
                atomic_load(&live.rx2_nonzero_slots));
        status = BLADERF_ERR_UNEXPECTED;
        goto cleanup;
    }
    printf("async RX epoch gate: PASS active=%s cross_band_cycles=%u "
           "valid=%u event_only=%u rx1_nonzero=%u rx2_nonzero=%u "
           "timestamp_discontinuities=%u timestamp_withheld=%u "
           "metadata_mismatch_withheld=%u "
           "overrun_events=%u history_gaps=%u epoch=%u\n",
           live.active_mode == LIVE_RX_ACTIVE_RX1 ? "RX1" :
               live.active_mode == LIVE_RX_ACTIVE_RX2 ? "RX2" : "BOTH",
           cross_band_cycles, atomic_load(&live.valid_callbacks),
           atomic_load(&live.event_only_callbacks),
           atomic_load(&live.rx1_nonzero_slots),
           atomic_load(&live.rx2_nonzero_slots),
           atomic_load(&live.timestamp_discontinuities),
           atomic_load(&live.timestamp_withheld_events),
           atomic_load(&live.metadata_mismatch_withheld_events),
           atomic_load(&live.stream_overrun_events),
           atomic_load(&live.event_history_gaps), event.epoch_id);
    status = 0;

cleanup:
    if (live.dev != NULL) {
        if (live.active_mode != LIVE_RX_ACTIVE_RX1) {
            bladerf_enable_module(live.dev, BLADERF_CHANNEL_RX(1), false);
        }
        if (live.active_mode != LIVE_RX_ACTIVE_RX2) {
            bladerf_enable_module(live.dev, BLADERF_CHANNEL_RX(0), false);
        }
        if (live.stream != NULL) {
            bladerf_deinit_stream(live.stream);
        }
        bladerf_close(live.dev);
    }
    return status == 0 ? 0 : 1;
}
