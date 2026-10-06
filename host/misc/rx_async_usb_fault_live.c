/* Test-only xA4 checks for terminal libusb RX failures and notifications. */
#define _POSIX_C_SOURCE 200809L
#include <libbladeRF.h>

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct fault_test {
    struct bladerf *dev;
    struct bladerf_stream *stream;
    void **buffers;
    atomic_uint data_callbacks;
    atomic_uint event_callbacks;
    atomic_uint withheld_events;
    atomic_uint overrun_events;
    atomic_bool fault_seen;
    atomic_bool data_after_fault;
    atomic_bool api_submit_requested;
    atomic_bool api_submit_mode;
    void *api_buffer;
    uint32_t expected_reason;
    int expected_stream_status;
    int api_submit_status;
    uint64_t event_cursor;
    int stream_status;
};

static void *rx_callback(struct bladerf *dev, struct bladerf_stream *stream,
                         struct bladerf_metadata *metadata, void *samples,
                         size_t num_samples, void *user_data)
{
    struct fault_test *test = user_data;
    (void)stream;
    (void)metadata;

    if (num_samples == 0) {
        struct bladerf_rf_event events[BLADERF_RF_EVENT_HISTORY_SIZE];
        uint32_t count = 0;
        uint64_t next = test->event_cursor;
        bool complete = false;
        if (samples != NULL || bladerf_rf_events_get_since(
                dev, test->event_cursor, events,
                BLADERF_RF_EVENT_HISTORY_SIZE, &count, &next,
                &complete) != 0 || !complete) {
            return BLADERF_STREAM_SHUTDOWN;
        }
        test->event_cursor = next;
        for (uint32_t i = 0; i < count; ++i) {
            if (events[i].event_type == BLADERF_RF_EVT_RX_DATA_WITHHELD &&
                events[i].flags == test->expected_reason) {
                atomic_fetch_add(&test->withheld_events, 1);
                atomic_store(&test->fault_seen, true);
            } else if (events[i].event_type ==
                       BLADERF_RF_EVT_RX_STREAM_OVERRUN) {
                atomic_fetch_add(&test->overrun_events, 1);
            }
        }
        atomic_fetch_add(&test->event_callbacks, 1);
        return BLADERF_STREAM_REUSE_BUFFER;
    }

    if (atomic_load(&test->fault_seen)) {
        atomic_store(&test->data_after_fault, true);
        return BLADERF_STREAM_SHUTDOWN;
    }
    if (atomic_load(&test->api_submit_mode)) {
        test->api_buffer = samples;
        atomic_store(&test->api_submit_requested, true);
        return BLADERF_STREAM_NO_DATA;
    }
    if (atomic_fetch_add(&test->data_callbacks, 1) >= 3) {
        return BLADERF_STREAM_SHUTDOWN;
    }
    return samples;
}

static void *run_stream(void *arg)
{
    struct fault_test *test = arg;
    test->stream_status = bladerf_stream(test->stream, BLADERF_RX_X2);
    return NULL;
}

static void *run_api_submit(void *arg)
{
    struct fault_test *test = arg;
    const struct timespec pause = { .tv_sec = 0, .tv_nsec = 1000000 };

    while (!atomic_load(&test->api_submit_requested) &&
           !atomic_load(&test->fault_seen)) {
        nanosleep(&pause, NULL);
    }
    if (atomic_load(&test->api_submit_requested)) {
        setenv("BLADERF_TEST_LIBUSB_RX_API_SUBMIT_ERROR", "IO", 1);
        test->api_submit_status = bladerf_submit_stream_buffer(
            test->stream, test->api_buffer, 2000);
        unsetenv("BLADERF_TEST_LIBUSB_RX_API_SUBMIT_ERROR");
    }
    return NULL;
}

int main(void)
{
    struct fault_test test = {0};
    pthread_t thread;
    const char *fault_status = getenv("BLADERF_TEST_LIBUSB_RX_STATUS");
    if (fault_status == NULL) {
        fault_status = "OVERFLOW";
    }
    if (strcmp(fault_status, "OVERFLOW") == 0) {
        test.expected_reason = BLADERF_RF_WITHHELD_USB_OVERFLOW;
        test.expected_stream_status = BLADERF_ERR_IO;
    } else if (strcmp(fault_status, "ERROR") == 0 ||
               strcmp(fault_status, "STALL") == 0) {
        test.expected_reason = BLADERF_RF_WITHHELD_USB_TRANSFER_ERROR;
        test.expected_stream_status = BLADERF_ERR_IO;
    } else if (strcmp(fault_status, "TIMEOUT") == 0) {
        test.expected_reason = BLADERF_RF_WITHHELD_USB_TIMEOUT;
        test.expected_stream_status = BLADERF_ERR_TIMEOUT;
    } else if (strcmp(fault_status, "NO_DEVICE") == 0) {
        test.expected_reason = BLADERF_RF_WITHHELD_DEVICE_LOST;
        test.expected_stream_status = BLADERF_ERR_NODEV;
    } else if (strcmp(fault_status, "EVENT_IO") == 0) {
        test.expected_reason = BLADERF_RF_WITHHELD_USB_TRANSFER_ERROR;
        test.expected_stream_status = BLADERF_ERR_IO;
    } else if (strcmp(fault_status, "SUBMIT_IO") == 0) {
        test.expected_reason = BLADERF_RF_WITHHELD_USB_TRANSFER_ERROR;
        test.expected_stream_status = BLADERF_ERR_IO;
    } else if (strcmp(fault_status, "CANCELLED") == 0) {
        test.expected_reason = BLADERF_RF_WITHHELD_USB_TRANSFER_ERROR;
        test.expected_stream_status = BLADERF_ERR_IO;
    } else if (strcmp(fault_status, "API_SUBMIT_IO") == 0) {
        test.expected_reason = BLADERF_RF_WITHHELD_USB_TRANSFER_ERROR;
        test.expected_stream_status = BLADERF_ERR_IO;
        atomic_store(&test.api_submit_mode, true);
    } else {
        fprintf(stderr, "unknown transfer status: %s\n", fault_status);
        return 2;
    }

    int status = bladerf_open(&test.dev, NULL);
    if (status != 0) {
        fprintf(stderr, "open failed: %s\n", bladerf_strerror(status));
        return 1;
    }

    status = bladerf_set_tuning_mode(test.dev, BLADERF_TUNING_MODE_HOST);
    if (status == 0) {
        status = bladerf_set_sample_rate(test.dev, BLADERF_CHANNEL_RX(0),
                                         4000000, NULL);
    }
    if (status == 0) {
        status = bladerf_set_sample_rate(test.dev, BLADERF_CHANNEL_RX(1),
                                         4000000, NULL);
    }
    if (status == 0) {
        status = bladerf_set_bandwidth(test.dev, BLADERF_CHANNEL_RX(0),
                                       5000000, NULL);
    }
    if (status == 0) {
        status = bladerf_set_bandwidth(test.dev, BLADERF_CHANNEL_RX(1),
                                       5000000, NULL);
    }
    if (status == 0) {
        status = bladerf_init_stream(&test.stream, test.dev, rx_callback,
                                     &test.buffers, 16,
                                     BLADERF_FORMAT_SC16_Q11_META,
                                     8192, 8, &test);
    }
    if (status == 0) {
        status = bladerf_enable_module(test.dev, BLADERF_CHANNEL_RX(0), true);
    }
    if (status == 0) {
        status = bladerf_enable_module(test.dev, BLADERF_CHANNEL_RX(1), true);
    }
    if (status == 0) {
        const struct bladerf_rx_transition_request request = {
            .target_frequency_hz = 1835000000ULL,
            .required_events_mask = BLADERF_RF_REQUIRE_PLL_LOCKED |
                                    BLADERF_RF_REQUIRE_ENSM_RX,
            .timeout_ms = 2000,
            .require_rx_data_valid = true,
            .epoch_settle_samples = 0,
        };
        uint32_t transaction_id = 0;
        struct bladerf_rf_event transition_event;
        status = bladerf_rx_transition_begin(
            test.dev, BLADERF_CHANNEL_RX(1), &request, &transaction_id);
        if (status == 0) {
            status = bladerf_rx_transition_wait(
                test.dev, transaction_id, &transition_event, 2000);
        }
    }
    if (status != 0) {
        fprintf(stderr, "RX setup failed: %s\n", bladerf_strerror(status));
        goto cleanup;
    }

    if (strcmp(fault_status, "API_SUBMIT_IO") == 0) {
        pthread_t submit_thread;
        if (pthread_create(&thread, NULL, run_stream, &test) != 0 ||
            pthread_create(&submit_thread, NULL, run_api_submit, &test) != 0) {
            fprintf(stderr, "could not start API submit fault test\n");
            status = BLADERF_ERR_UNEXPECTED;
            goto cleanup;
        }
        pthread_join(submit_thread, NULL);
        pthread_join(thread, NULL);
    } else if ((strcmp(fault_status, "EVENT_IO") == 0
             ? setenv("BLADERF_TEST_LIBUSB_RX_EVENT_ERROR", "IO", 1)
         : strcmp(fault_status, "SUBMIT_IO") == 0
             ? setenv("BLADERF_TEST_LIBUSB_RX_SUBMIT_ERROR", "IO", 1)
             : setenv("BLADERF_TEST_LIBUSB_RX_STATUS", fault_status, 1)) != 0 ||
        pthread_create(&thread, NULL, run_stream, &test) != 0) {
        fprintf(stderr, "could not arm/start test stream\n");
        status = BLADERF_ERR_UNEXPECTED;
        goto cleanup;
    }

    pthread_join(thread, NULL);
    unsetenv("BLADERF_TEST_LIBUSB_RX_STATUS");
    unsetenv("BLADERF_TEST_LIBUSB_RX_EVENT_ERROR");
    unsetenv("BLADERF_TEST_LIBUSB_RX_SUBMIT_ERROR");
    unsetenv("BLADERF_TEST_LIBUSB_RX_API_SUBMIT_ERROR");

    const bool event_loop_fault = strcmp(fault_status, "EVENT_IO") == 0;
    const bool pre_callback_fault = event_loop_fault ||
                                    strcmp(fault_status, "API_SUBMIT_IO") == 0 ||
                                    strcmp(fault_status, "SUBMIT_IO") == 0;
    if (test.stream_status != test.expected_stream_status ||
        (strcmp(fault_status, "API_SUBMIT_IO") == 0 &&
         test.api_submit_status != BLADERF_ERR_IO) ||
        atomic_load(&test.withheld_events) != 1 ||
        (pre_callback_fault ? atomic_load(&test.overrun_events) < 1
                            : atomic_load(&test.overrun_events) != 1) ||
        (pre_callback_fault ? atomic_load(&test.event_callbacks) < 1
                            : atomic_load(&test.event_callbacks) != 1) ||
        atomic_load(&test.data_after_fault)) {
        fprintf(stderr, "FAIL stream=%s data=%u event_only=%u withheld=%u "
                "overrun=%u data_after_fault=%u api_submit=%s expected_reason=0x%x\n",
                bladerf_strerror(test.stream_status),
                atomic_load(&test.data_callbacks),
                atomic_load(&test.event_callbacks),
                atomic_load(&test.withheld_events),
                atomic_load(&test.overrun_events),
                atomic_load(&test.data_after_fault),
                bladerf_strerror(test.api_submit_status), test.expected_reason);
        status = BLADERF_ERR_UNEXPECTED;
    } else {
        printf("PASS libusb RX_X2 %s callback: data=%u event_only=%u "
               "withheld=%u overrun=%u post_fault_IQ=0 stream=%s\n",
               fault_status,
               atomic_load(&test.data_callbacks),
               atomic_load(&test.event_callbacks),
               atomic_load(&test.withheld_events),
               atomic_load(&test.overrun_events),
               bladerf_strerror(test.stream_status));
        status = 0;
    }

cleanup:
    if (test.dev != NULL) {
        unsetenv("BLADERF_TEST_LIBUSB_RX_STATUS");
        unsetenv("BLADERF_TEST_LIBUSB_RX_EVENT_ERROR");
        unsetenv("BLADERF_TEST_LIBUSB_RX_SUBMIT_ERROR");
        unsetenv("BLADERF_TEST_LIBUSB_RX_API_SUBMIT_ERROR");
        bladerf_enable_module(test.dev, BLADERF_CHANNEL_RX(0), false);
        bladerf_enable_module(test.dev, BLADERF_CHANNEL_RX(1), false);
        if (test.stream != NULL) {
            bladerf_deinit_stream(test.stream);
        }
        bladerf_close(test.dev);
    }
    return status == 0 ? 0 : 1;
}
