/* Prove epoch-required transitions reject an active async RX stream whose
 * format has no per-packet epoch identity. */
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include <libbladeRF.h>

#define RX_BUFFERS 8
#define RX_SAMPLES 4096

struct live_test {
    struct bladerf *dev;
    struct bladerf_stream *stream;
    void **buffers;
    atomic_bool stop;
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

    if (atomic_load(&test->stop)) {
        return BLADERF_STREAM_SHUTDOWN;
    }
    if (samples == NULL || num_samples == 0) {
        return test->buffers[0];
    }
    atomic_fetch_add(&test->callbacks, 1);
    return samples;
}

static void *run_stream(void *arg)
{
    struct live_test *test = arg;
    test->stream_status = bladerf_stream(test->stream, BLADERF_RX_X1);
    return NULL;
}

static bool wait_for_callback(struct live_test *test)
{
    struct timespec pause = { .tv_sec = 0, .tv_nsec = 10000000 };
    for (unsigned int i = 0; i < 300; ++i) {
        if (atomic_load(&test->callbacks) != 0) {
            return true;
        }
        nanosleep(&pause, NULL);
    }
    return false;
}

static int request_epoch(struct bladerf *dev, uint32_t *transaction_id)
{
    const struct bladerf_rx_transition_request request = {
        .target_frequency_hz = 1835400000ULL,
        .required_events_mask = BLADERF_RF_REQUIRE_EPOCH_VALID,
        .timeout_ms = 2000,
        .require_rx_data_valid = true,
    };
    return bladerf_rx_transition_begin(dev, BLADERF_CHANNEL_RX(0), &request,
                                       transaction_id);
}

int main(void)
{
    struct live_test test = {0};
    pthread_t stream_thread;
    uint32_t transaction_id = 0;
    uint64_t frequency_before = 0, frequency_after = 0;
    int status;

    status = bladerf_open(&test.dev, NULL);
    if (status != 0) {
        fprintf(stderr, "open: %s\n", bladerf_strerror(status));
        return 1;
    }
    status = bladerf_set_tuning_mode(test.dev, BLADERF_TUNING_MODE_HOST);
    if (status == 0) {
        status = bladerf_set_sample_rate(test.dev, BLADERF_CHANNEL_RX(0),
                                         4000000, NULL);
    }
    if (status == 0) {
        status = bladerf_set_bandwidth(test.dev, BLADERF_CHANNEL_RX(0),
                                       5000000, NULL);
    }
    if (status == 0) {
        status = bladerf_set_frequency(test.dev, BLADERF_CHANNEL_RX(0),
                                       1835000000ULL);
    }
    if (status == 0) {
        status = bladerf_enable_module(test.dev, BLADERF_CHANNEL_RX(0), true);
    }
    if (status == 0) {
        status = bladerf_init_stream(&test.stream, test.dev, rx_callback,
                                     &test.buffers, RX_BUFFERS,
                                     BLADERF_FORMAT_SC16_Q11, RX_SAMPLES,
                                     4, &test);
    }
    if (status != 0) {
        fprintf(stderr, "setup: %s\n", bladerf_strerror(status));
        goto fail;
    }
    if (pthread_create(&stream_thread, NULL, run_stream, &test) != 0) {
        fprintf(stderr, "failed to start RX stream thread\n");
        goto fail;
    }
    if (!wait_for_callback(&test)) {
        fprintf(stderr, "raw async stream produced no callback\n");
        atomic_store(&test.stop, true);
        (void)bladerf_submit_stream_buffer(test.stream,
                                          BLADERF_STREAM_SHUTDOWN, 1000);
        pthread_join(stream_thread, NULL);
        goto fail;
    }

    status = bladerf_get_frequency(test.dev, BLADERF_CHANNEL_RX(0),
                                   &frequency_before);
    if (status != 0) {
        fprintf(stderr, "frequency before: %s\n", bladerf_strerror(status));
        goto stop_fail;
    }
    status = request_epoch(test.dev, &transaction_id);
    if (status != BLADERF_ERR_UNSUPPORTED) {
        fprintf(stderr, "epoch transition status=%d (%s), expected %d\n",
                status, bladerf_strerror(status), BLADERF_ERR_UNSUPPORTED);
        goto stop_fail;
    }
    status = bladerf_get_frequency(test.dev, BLADERF_CHANNEL_RX(0),
                                   &frequency_after);
    if (status != 0 || frequency_before != frequency_after) {
        fprintf(stderr, "rejected transition changed LO (%llu -> %llu)\n",
                (unsigned long long)frequency_before,
                (unsigned long long)frequency_after);
        goto stop_fail;
    }

    atomic_store(&test.stop, true);
    (void)bladerf_submit_stream_buffer(test.stream, BLADERF_STREAM_SHUTDOWN,
                                      1000);
    pthread_join(stream_thread, NULL);
    bladerf_deinit_stream(test.stream);
    test.stream = NULL;

    status = request_epoch(test.dev, &transaction_id);
    if (status != 0) {
        fprintf(stderr, "epoch transition after raw stream stop: %s\n",
                bladerf_strerror(status));
        goto fail;
    }
    struct bladerf_rf_event final_event = {0};
    status = bladerf_rx_transition_wait(test.dev, transaction_id,
                                        &final_event, 3000);
    if (status != 0 || final_event.fpga_state != BLADERF_RF_STATE_RX_DATA_VALID) {
        fprintf(stderr, "post-stop transition failed: status=%d state=%d\n",
                status, final_event.fpga_state);
        goto fail;
    }

    status = bladerf_get_frequency(test.dev, BLADERF_CHANNEL_RX(0),
                                   &frequency_after);
    if (status != 0 ||
        (frequency_after > 1835400000ULL ?
             frequency_after - 1835400000ULL :
             1835400000ULL - frequency_after) > 1000ULL) {
        fprintf(stderr, "post-stop transition LO mismatch: status=%d LO=%llu\n",
                status, (unsigned long long)frequency_after);
        goto fail;
    }
    printf("async epoch format guard: PASS raw callbacks=%u rejected=%d \
LO after rejected request=%llu; accepted retune LO=%llu transaction=%u\n",
           atomic_load(&test.callbacks), BLADERF_ERR_UNSUPPORTED,
           (unsigned long long)frequency_before,
           (unsigned long long)frequency_after, transaction_id);
    bladerf_enable_module(test.dev, BLADERF_CHANNEL_RX(0), false);
    bladerf_close(test.dev);
    return 0;

stop_fail:
    atomic_store(&test.stop, true);
    (void)bladerf_submit_stream_buffer(test.stream, BLADERF_STREAM_SHUTDOWN,
                                      1000);
    pthread_join(stream_thread, NULL);
fail:
    if (test.stream != NULL) {
        bladerf_deinit_stream(test.stream);
    }
    bladerf_close(test.dev);
    return 2;
}
