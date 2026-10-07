/* Verify epoch-required async stream starts reject non-epoch RX formats and
 * retain one reason-coded event even though no USB callback is started. */
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <libbladeRF.h>

#define RX_BUFFERS 8
#define RX_SAMPLES 4096

struct live_test {
    struct bladerf *dev;
    struct bladerf_stream *stream;
    void **buffers;
    atomic_uint callbacks;
    int stream_status;
};

static void *rx_callback(struct bladerf *dev, struct bladerf_stream *stream,
                         struct bladerf_metadata *metadata, void *samples,
                         size_t num_samples, void *user_data)
{
    struct live_test *test = user_data;
    (void)dev;
    (void)stream;
    (void)metadata;
    (void)num_samples;
    atomic_fetch_add(&test->callbacks, 1);
    return samples != NULL ? samples : test->buffers[0];
}

static void *run_stream(void *arg)
{
    struct live_test *test = arg;
    test->stream_status = bladerf_stream(test->stream, BLADERF_RX_X1);
    return NULL;
}

static int transition(struct bladerf *dev, bladerf_channel channel)
{
    struct bladerf_rx_transition_request request = {0};
    struct bladerf_rf_event final_event = {0};
    uint32_t transaction_id = 0;
    request.target_frequency_hz = 1835000000ULL;
    request.required_events_mask = BLADERF_RF_REQUIRE_EPOCH_VALID;
    request.require_rx_data_valid = true;
    request.timeout_ms = 3000;
    int status = bladerf_rx_transition_begin(dev, channel, &request,
                                            &transaction_id);
    if (status == 0) {
        status = bladerf_rx_transition_wait(dev, transaction_id, &final_event,
                                            3000);
    }
    if (status == 0 &&
        final_event.fpga_state != BLADERF_RF_STATE_RX_DATA_VALID) {
        status = BLADERF_ERR_UNEXPECTED;
    }
    return status;
}

static int verify_rejected_format(struct live_test *test,
                                  bladerf_channel_layout layout,
                                  bladerf_format format)
{
    struct bladerf_rf_event events[BLADERF_RF_EVENT_HISTORY_SIZE];
    uint32_t event_count = 0;
    uint64_t cursor = 0, next_cursor = 0;
    bool history_complete = false;
    pthread_t thread;
    unsigned int callbacks_before = atomic_load(&test->callbacks);
    int status = bladerf_rf_events_get_since(
        test->dev, 0, events, BLADERF_RF_EVENT_HISTORY_SIZE, &event_count,
        &cursor, &history_complete);
    if (status != 0 || !history_complete) {
        return status != 0 ? status : BLADERF_ERR_UNEXPECTED;
    }

    status = bladerf_init_stream(&test->stream, test->dev, rx_callback,
                                 &test->buffers, RX_BUFFERS, format,
                                 RX_SAMPLES, 4, test);
    if (status != 0) {
        return status;
    }
    if (pthread_create(&thread, NULL, run_stream, test) != 0) {
        bladerf_deinit_stream(test->stream);
        test->stream = NULL;
        return BLADERF_ERR_UNEXPECTED;
    }
    pthread_join(thread, NULL);

    if (test->stream_status != BLADERF_ERR_UNSUPPORTED ||
        atomic_load(&test->callbacks) != callbacks_before) {
        fprintf(stderr, "format=%d stream status=%d callbacks=%u before=%u\n",
                format, test->stream_status,
                atomic_load(&test->callbacks), callbacks_before);
        status = BLADERF_ERR_UNEXPECTED;
        goto done;
    }

    status = bladerf_rf_events_get_since(
        test->dev, cursor, events, BLADERF_RF_EVENT_HISTORY_SIZE,
        &event_count, &next_cursor, &history_complete);
    if (status != 0 || !history_complete) {
        status = status != 0 ? status : BLADERF_ERR_UNEXPECTED;
        goto done;
    }
    bool found = false;
    for (uint32_t i = 0; i < event_count; ++i) {
        if (events[i].event_type == BLADERF_RF_EVT_RX_FORMAT_UNSUPPORTED &&
            events[i].flags == (uint32_t)format &&
            events[i].error_code == BLADERF_ERR_UNSUPPORTED) {
            found = true;
            break;
        }
    }
    if (!found) {
        fprintf(stderr, "missing format rejection event for format=%d\n",
                format);
        status = BLADERF_ERR_UNEXPECTED;
    } else {
        printf("async start rejected format=%d layout=%d before callbacks\n",
               format, layout);
        status = 0;
    }

done:
    bladerf_deinit_stream(test->stream);
    test->stream = NULL;
    return status;
}

int main(int argc, char **argv)
{
    struct live_test test = {0};
    bladerf_channel channel = BLADERF_CHANNEL_RX(0);
    bladerf_channel_layout layout = BLADERF_RX_X1;
    int status;
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "RX1") != 0 &&
                                  strcmp(argv[1], "RX2") != 0)) {
        fprintf(stderr, "usage: %s [RX1|RX2]\n", argv[0]);
        return 2;
    }
    if (argc == 2 && strcmp(argv[1], "RX2") == 0) {
        channel = BLADERF_CHANNEL_RX(1);
    }

    status = bladerf_open(&test.dev, NULL);
    if (status == 0) {
        status = bladerf_set_tuning_mode(test.dev, BLADERF_TUNING_MODE_HOST);
    }
    if (status == 0) {
        status = bladerf_set_sample_rate(test.dev, channel, 4000000, NULL);
    }
    if (status == 0) {
        status = bladerf_set_bandwidth(test.dev, channel, 5000000, NULL);
    }
    if (status == 0) {
        status = bladerf_enable_module(test.dev, channel, true);
    }
    if (status == 0) {
        status = transition(test.dev, channel);
    }
    if (status == 0) {
        status = verify_rejected_format(
            &test, layout, BLADERF_FORMAT_PACKET_META);
    }
    if (status == 0) {
        status = verify_rejected_format(
            &test, layout, BLADERF_FORMAT_SC16_Q11);
    }
    if (status != 0) {
        fprintf(stderr, "async format preflight failed: %s (%d)\n",
                bladerf_strerror(status), status);
    }
    if (test.stream != NULL) {
        bladerf_deinit_stream(test.stream);
    }
    if (test.dev != NULL) {
        (void)bladerf_enable_module(test.dev, channel, false);
        bladerf_close(test.dev);
    }
    return status == 0 ? 0 : 1;
}
