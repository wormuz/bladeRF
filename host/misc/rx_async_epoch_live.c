/* Live xA4 check: async RX must withhold IQ across invalid/cross-epoch data. */
#include <libbladeRF.h>

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

struct live_stream {
    struct bladerf *dev;
    struct bladerf_stream *stream;
    void **buffers;
    atomic_uint valid_callbacks;
    atomic_uint event_only_callbacks;
    atomic_bool stop;
    atomic_int event_query_status;
    uint64_t event_cursor;
    atomic_uint events_drained_from_callback;
    atomic_uint data_withheld_events;
    atomic_uint event_history_gaps;
    atomic_bool pause_event_poll;
    int stream_status;
};

static void *rx_callback(struct bladerf *dev, struct bladerf_stream *stream,
                         struct bladerf_metadata *metadata, void *samples,
                         size_t num_samples, void *user_data)
{
    struct live_stream *live = user_data;
    (void)stream;
    (void)metadata;

    if (atomic_load(&live->stop)) {
        return BLADERF_STREAM_SHUTDOWN;
    }
    if (num_samples == 0) {
        if (samples != NULL) {
            return BLADERF_STREAM_SHUTDOWN;
        }
        if (!atomic_load(&live->pause_event_poll)) {
            struct bladerf_rf_event events[BLADERF_RF_EVENT_HISTORY_SIZE];
            uint32_t event_count = 0;
            uint64_t next_sequence = live->event_cursor;
            bool history_complete = false;
            int query_status = bladerf_rf_events_get_since(
                dev, live->event_cursor, events,
                BLADERF_RF_EVENT_HISTORY_SIZE, &event_count, &next_sequence,
                &history_complete);
            if (query_status != 0 && history_complete) {
                atomic_store(&live->event_query_status, query_status);
            } else {
                live->event_cursor = next_sequence;
                atomic_fetch_add(&live->events_drained_from_callback,
                                 event_count);
                if (!history_complete) {
                    atomic_fetch_add(&live->event_history_gaps, 1);
                }
                for (uint32_t i = 0; i < event_count; ++i) {
                    if (events[i].event_type ==
                        BLADERF_RF_EVT_RX_DATA_WITHHELD) {
                        atomic_fetch_add(&live->data_withheld_events, 1);
                    }
                }
            }
        }
        atomic_fetch_add(&live->event_only_callbacks, 1);
        /* The library recycles the withheld transfer after this wakeup. */
        return live->buffers[0];
    }
    if (samples == NULL) {
        return BLADERF_STREAM_SHUTDOWN;
    }
    atomic_fetch_add(&live->valid_callbacks, 1);
    return samples;
}

static void *run_stream(void *arg)
{
    struct live_stream *live = arg;
    live->stream_status = bladerf_stream(live->stream, BLADERF_RX_X2);
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
    uint32_t transaction_id;
    int status = bladerf_rx_transition_begin(
        dev, BLADERF_CHANNEL_RX(1), &request, &transaction_id);
    if (status == 0) {
        status = bladerf_rx_transition_wait(dev, transaction_id, result, 2000);
    }
    if (status == 0 &&
        (result->event_type != BLADERF_RF_EVT_RX_EPOCH_VALID ||
         result->fpga_state != BLADERF_RF_STATE_RX_DATA_VALID)) {
        return BLADERF_ERR_UNEXPECTED;
    }
    return status;
}

int main(void)
{
    struct live_stream live = {0};
    pthread_t stream_thread;
    struct bladerf_rf_event event = {0};
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
                                     8192, 8, &live);
    }
    if (status == 0) {
        status = bladerf_enable_module(live.dev, BLADERF_CHANNEL_RX(0), true);
    }
    if (status == 0) {
        status = bladerf_enable_module(live.dev, BLADERF_CHANNEL_RX(1), true);
    }
    if (status == 0) {
        status = event_transition(live.dev, 1835000000ULL, &event);
    }
    if (status != 0) {
        fprintf(stderr, "async RX setup/initial transition: %s\n",
                bladerf_strerror(status));
        goto cleanup;
    }
    fprintf(stderr, "initial epoch certified: %u\n", event.epoch_id);

    if (pthread_create(&stream_thread, NULL, run_stream, &live) != 0) {
        fprintf(stderr, "pthread_create failed\n");
        status = BLADERF_ERR_UNEXPECTED;
        goto cleanup;
    }
    fprintf(stderr, "async stream started\n");
    if (!wait_for_count(&live.valid_callbacks, 2, 3000)) {
        fprintf(stderr, "no certified async RX buffers reached callback\n");
        status = BLADERF_ERR_TIMEOUT;
        stop_stream(&live, stream_thread);
        goto cleanup;
    }
    fprintf(stderr, "initial valid callbacks: %u\n",
            atomic_load(&live.valid_callbacks));

    /* Same-value configuration still invalidates a prior RX certificate. */
    fprintf(stderr, "issuing invalidating gain setter\n");
    status = bladerf_set_gain(live.dev, BLADERF_CHANNEL_RX(1), 30);
    fprintf(stderr, "gain setter returned %d\n", status);
    if (status != 0 ||
        !wait_for_count(&live.event_only_callbacks, 2, 3000)) {
        fprintf(stderr, "invalidation did not withhold async IQ: %s\n",
                bladerf_strerror(status));
        status = status != 0 ? status : BLADERF_ERR_UNEXPECTED;
        stop_stream(&live, stream_thread);
        goto cleanup;
    }
    fprintf(stderr, "event-only callbacks after invalidation: %u\n",
            atomic_load(&live.event_only_callbacks));
    if (atomic_load(&live.data_withheld_events) == 0) {
        fprintf(stderr, "native RX_DATA_WITHHELD event was not observed\n");
        status = BLADERF_ERR_UNEXPECTED;
        stop_stream(&live, stream_thread);
        goto cleanup;
    }

    unsigned int valid_before = atomic_load(&live.valid_callbacks);
    status = event_transition(live.dev, 1835400000ULL, &event);
    if (status != 0 ||
        !wait_for_count(&live.valid_callbacks, valid_before + 2, 3000)) {
        fprintf(stderr, "new async epoch did not restore callback IQ: %s\n",
                bladerf_strerror(status));
        status = status != 0 ? status : BLADERF_ERR_UNEXPECTED;
        stop_stream(&live, stream_thread);
        goto cleanup;
    }

    /* Overflow the bounded history while paired RX1+RX2 async traffic keeps
     * moving. Resuming callback polling must report the missing cursor range. */
    atomic_store(&live.pause_event_poll, true);
    unsigned int wrap_valid_target = atomic_load(&live.valid_callbacks);
    unsigned int wrap_event_target = atomic_load(&live.event_only_callbacks);
    for (unsigned int i = 0; i < 70; ++i) {
        status = bladerf_set_gain(live.dev, BLADERF_CHANNEL_RX(1), 30);
        if (status != 0 ||
            !wait_for_count(&live.event_only_callbacks, ++wrap_event_target,
                            3000)) {
            fprintf(stderr, "ring-wrap invalidation %u failed: %s\n", i,
                    bladerf_strerror(status));
            status = status != 0 ? status : BLADERF_ERR_UNEXPECTED;
            stop_stream(&live, stream_thread);
            goto cleanup;
        }
        /* Alternate VCO bands on every transition: 947.5 MHz and 1835 MHz. */
        const uint64_t target_frequency = (i % 2) == 0 ?
            947500000ULL : 1835000000ULL;
        status = event_transition(live.dev, target_frequency, &event);
        if (status != 0 ||
            !wait_for_count(&live.valid_callbacks, ++wrap_valid_target, 3000)) {
            fprintf(stderr, "ring-wrap recovery %u failed: %s\n", i,
                    bladerf_strerror(status));
            status = status != 0 ? status : BLADERF_ERR_UNEXPECTED;
            stop_stream(&live, stream_thread);
            goto cleanup;
        }
        if ((i + 1) % 10 == 0) {
            fprintf(stderr, "paired cross-band invalidation/recovery=%u/70\n",
                    i + 1);
        }
    }

    /* Trigger a callback-side read after wrap; it must surface an incomplete
     * cursor rather than silently treating the retained tail as complete. */
    atomic_store(&live.pause_event_poll, false);
    status = bladerf_set_gain(live.dev, BLADERF_CHANNEL_RX(1), 30);
    if (status != 0 ||
        !wait_for_count(&live.event_history_gaps, 1, 3000)) {
        fprintf(stderr, "callback did not report overwritten RF history: %s\n",
                bladerf_strerror(status));
        status = status != 0 ? status : BLADERF_ERR_UNEXPECTED;
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

    printf("async RX epoch gate: PASS valid=%u event_only=%u history_gaps=%u "
           "epoch=%u\n",
           atomic_load(&live.valid_callbacks),
           atomic_load(&live.event_only_callbacks),
           atomic_load(&live.event_history_gaps), event.epoch_id);
    if (atomic_load(&live.event_query_status) != 0 ||
        atomic_load(&live.events_drained_from_callback) == 0) {
        fprintf(stderr, "callback-side RF event history query failed\n");
        status = BLADERF_ERR_UNEXPECTED;
        goto cleanup;
    }
    status = 0;

cleanup:
    if (live.dev != NULL) {
        bladerf_enable_module(live.dev, BLADERF_CHANNEL_RX(1), false);
        bladerf_enable_module(live.dev, BLADERF_CHANNEL_RX(0), false);
        if (live.stream != NULL) {
            bladerf_deinit_stream(live.stream);
        }
        bladerf_close(live.dev);
    }
    return status == 0 ? 0 : 1;
}
