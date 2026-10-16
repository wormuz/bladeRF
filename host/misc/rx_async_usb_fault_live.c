/* Test-only xA4 checks for terminal libusb RX failures and notifications. */
#define _POSIX_C_SOURCE 200809L
#include <libbladeRF.h>
#include "timestamp_order.h"

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "host_config.h"
#include "streaming/async.h"
#include "streaming/metadata.h"

#define RX_META_MESSAGE_BYTES 8192u
#define RX_X2_IQ_COMPLEX_PER_TIMESTAMP 2u
#define RX_WITHHELD_REASON_MASK \
    (BLADERF_RF_WITHHELD_EPOCH_UNCERTIFIED | \
     BLADERF_RF_WITHHELD_EPOCH_OR_TIMESTAMP_MISMATCH | \
     BLADERF_RF_WITHHELD_TIMESTAMP_DISCONTINUITY | \
     BLADERF_RF_WITHHELD_SHORT_TRANSFER | \
     BLADERF_RF_WITHHELD_USB_OVERFLOW | \
     BLADERF_RF_WITHHELD_USB_TRANSFER_ERROR | \
     BLADERF_RF_WITHHELD_USB_TIMEOUT | \
     BLADERF_RF_WITHHELD_DEVICE_LOST | \
     BLADERF_RF_WITHHELD_SYNC_TIMEOUT | \
     BLADERF_RF_WITHHELD_RX_CHANNEL_SELECTION)

enum rx_fault_layout {
    RX_FAULT_LAYOUT_X2,
    RX_FAULT_LAYOUT_RX1,
    RX_FAULT_LAYOUT_RX2,
};

struct fault_test {
    struct bladerf *dev;
    struct bladerf_stream *stream;
    enum rx_fault_layout layout;
    bladerf_channel transition_channel;
    const char *fault_status;
    void **buffers;
    atomic_uint data_callbacks;
    atomic_uint event_callbacks;
    atomic_uint withheld_events;
    atomic_uint overrun_events;
    atomic_uint resumed_events;
    atomic_uint first_valid_events;
    atomic_uint resume_metadata_mismatches;
    atomic_bool withheld_timestamp_valid;
    atomic_uint_fast64_t withheld_timestamp;
    atomic_uint_fast64_t resumed_timestamp;
    atomic_bool fault_seen;
    atomic_bool data_after_fault;
    atomic_bool expected_epoch_ready;
    atomic_bool fault_injection_armed;
    atomic_uint pre_fault_data_callbacks;
    atomic_uint post_fault_data_callbacks;
    atomic_bool api_submit_requested;
    atomic_bool api_submit_mode;
    atomic_bool recoverable_short_mode;
    atomic_bool short_after_valid_mode;
    atomic_uint rx1_samples;
    atomic_uint rx2_samples;
    atomic_uint invalid_meta_buffers;
    void *api_buffer;
    uint32_t expected_reason;
    int expected_stream_status;
    uint8_t expected_epoch_id;
    uint64_t first_valid_timestamp;
    int api_submit_status;
    uint64_t event_cursor;
    int stream_status;
};

static bool consume_rf_events(struct fault_test *test, struct bladerf *dev)
{
    struct bladerf_rf_event events[BLADERF_RF_EVENT_HISTORY_SIZE];
    uint32_t count = 0;
    uint64_t next = test->event_cursor;
    bool complete = false;

    if (bladerf_rf_events_get_since(dev, test->event_cursor, events,
            BLADERF_RF_EVENT_HISTORY_SIZE, &count, &next, &complete) != 0 ||
        !complete) {
        return false;
    }
    test->event_cursor = next;
    for (uint32_t i = 0; i < count; ++i) {
        if (events[i].event_type == BLADERF_RF_EVT_RX_DATA_WITHHELD &&
            (events[i].flags & RX_WITHHELD_REASON_MASK) ==
                test->expected_reason) {
            atomic_fetch_add(&test->withheld_events, 1);
            atomic_store(&test->fault_seen, true);
            if ((events[i].flags &
                 BLADERF_RF_EVENT_F_FPGA_TIMESTAMP_VALID) != 0) {
                atomic_store(&test->withheld_timestamp_valid, true);
                atomic_store(&test->withheld_timestamp,
                             events[i].fpga_timestamp);
            }
        } else if (events[i].event_type == BLADERF_RF_EVT_RX_STREAM_OVERRUN) {
            atomic_fetch_add(&test->overrun_events, 1);
        } else if (events[i].event_type == BLADERF_RF_EVT_RX_DATA_RESUMED) {
            if (events[i].epoch_id != test->expected_epoch_id ||
                !timestamp_at_or_after(events[i].fpga_timestamp,
                                       test->first_valid_timestamp) ||
                (events[i].flags &
                 BLADERF_RF_EVENT_F_FPGA_TIMESTAMP_VALID) == 0) {
                atomic_fetch_add(&test->resume_metadata_mismatches, 1);
            }
            atomic_store(&test->resumed_timestamp,
                         events[i].fpga_timestamp);
            atomic_fetch_add(&test->resumed_events, 1);
        } else if (events[i].event_type ==
                   BLADERF_RF_EVT_RX_FIRST_VALID_HOST_DATA) {
            if ((events[i].flags &
                 BLADERF_RF_EVENT_F_FPGA_TIMESTAMP_VALID) == 0 ||
                !timestamp_at_or_after(events[i].fpga_timestamp,
                                       test->first_valid_timestamp)) {
                atomic_fetch_add(&test->resume_metadata_mismatches, 1);
            }
            atomic_fetch_add(&test->first_valid_events, 1);
        }
    }
    return true;
}

static void arm_rx_fault(struct fault_test *test)
{
    if (atomic_exchange(&test->fault_injection_armed, true)) {
        return;
    }

    const char *fault = test->fault_status;
    if (strcmp(fault, "EVENT_IO") == 0 ||
        strcmp(fault, "EVENT_NODEV") == 0) {
        (void)setenv("BLADERF_TEST_LIBUSB_RX_EVENT_ERROR",
                     strcmp(fault, "EVENT_NODEV") == 0 ? "NO_DEVICE" : "IO",
                     1);
    } else if (strcmp(fault, "SUBMIT_IO") == 0 ||
               strcmp(fault, "SUBMIT_NODEV") == 0 ||
               strcmp(fault, "SUBMIT_TIMEOUT") == 0) {
        const char *status = strcmp(fault, "SUBMIT_NODEV") == 0 ? "NO_DEVICE" :
            strcmp(fault, "SUBMIT_TIMEOUT") == 0 ? "TIMEOUT" : "IO";
        (void)setenv("BLADERF_TEST_LIBUSB_RX_SUBMIT_ERROR", status, 1);
    } else if (strcmp(fault, "API_SUBMIT_IO") != 0) {
        (void)setenv("BLADERF_TEST_LIBUSB_RX_STATUS",
                     strcmp(fault, "SHORT_AFTER_VALID") == 0
                         ? "SHORT" : fault,
                     1);
        if (strcmp(fault, "SHORT_AFTER_VALID") == 0) {
            (void)setenv("BLADERF_TEST_LIBUSB_RX_STATUS_AFTER_COMPLETIONS",
                         "20", 1);
        }
    }
}

static void *rx_callback(struct bladerf *dev, struct bladerf_stream *stream,
                         struct bladerf_metadata *metadata, void *samples,
                         size_t num_samples, void *user_data)
{
    struct fault_test *test = user_data;
    (void)stream;
    (void)metadata;

    if (num_samples == 0) {
        if (samples != NULL || !consume_rf_events(test, dev)) {
            return BLADERF_STREAM_SHUTDOWN;
        }
        atomic_fetch_add(&test->event_callbacks, 1);
        return BLADERF_STREAM_REUSE_BUFFER;
    }

    /* The stream is started before the RX epoch transition so USB has no
     * stale pre-epoch queue to drain. Do not consume a valid callback until
     * the control thread publishes the epoch returned by transition_wait. */
    if (!atomic_load(&test->expected_epoch_ready)) {
        return BLADERF_STREAM_REUSE_BUFFER;
    }

    const bool fault_seen = atomic_load(&test->fault_seen);
    if (fault_seen) {
        if (!atomic_load(&test->recoverable_short_mode)) {
            atomic_store(&test->data_after_fault, true);
            return BLADERF_STREAM_SHUTDOWN;
        }
    } else {
        atomic_fetch_add(&test->pre_fault_data_callbacks, 1);
    }
    if (atomic_load(&test->api_submit_mode)) {
        test->api_buffer = samples;
        atomic_store(&test->api_submit_requested, true);
        return BLADERF_STREAM_NO_DATA;
    }
    const size_t received_bytes = num_samples * sizeof(uint32_t);
    if (received_bytes == 0 || received_bytes % RX_META_MESSAGE_BYTES != 0) {
        atomic_fetch_add(&test->invalid_meta_buffers, 1);
        return BLADERF_STREAM_SHUTDOWN;
    }
    for (size_t offset = 0; offset < received_bytes;
         offset += RX_META_MESSAGE_BYTES) {
        const uint8_t *message = (const uint8_t *)samples + offset;
        uint8_t epoch_id = 0;
        if (!metadata_get_rx_epoch_id(message, &epoch_id) ||
            epoch_id != test->expected_epoch_id ||
            !timestamp_at_or_after(metadata_get_timestamp(message),
                                   test->first_valid_timestamp)) {
            atomic_fetch_add(&test->invalid_meta_buffers, 1);
            return BLADERF_STREAM_SHUTDOWN;
        }
        const size_t payload_bytes =
            RX_META_MESSAGE_BYTES - METADATA_HEADER_SIZE;
        const unsigned complex_samples = (unsigned)(
            payload_bytes / sizeof(uint32_t));
        if (test->layout == RX_FAULT_LAYOUT_X2) {
            const unsigned channel_samples =
                complex_samples / RX_X2_IQ_COMPLEX_PER_TIMESTAMP;
            atomic_fetch_add(&test->rx1_samples, channel_samples);
            atomic_fetch_add(&test->rx2_samples, channel_samples);
        } else if (test->layout == RX_FAULT_LAYOUT_RX1) {
            atomic_fetch_add(&test->rx1_samples, complex_samples);
        } else {
            atomic_fetch_add(&test->rx2_samples, complex_samples);
        }
    }
    if (!consume_rf_events(test, dev)) {
        return BLADERF_STREAM_SHUTDOWN;
    }
    atomic_fetch_add(&test->data_callbacks, 1);
    if (atomic_load(&test->recoverable_short_mode) &&
        atomic_load(&test->fault_seen)) {
        const unsigned recovered = atomic_fetch_add(
            &test->post_fault_data_callbacks, 1) + 1;
        if (recovered >= (atomic_load(&test->short_after_valid_mode) ? 50u : 1u)) {
            return BLADERF_STREAM_SHUTDOWN;
        }
    } else if (!atomic_load(&test->fault_seen) &&
               atomic_load(&test->pre_fault_data_callbacks) >=
                   (atomic_load(&test->short_after_valid_mode) ? 20u : 3u)) {
        arm_rx_fault(test);
    }
    return samples;
}

static void *run_stream(void *arg)
{
    struct fault_test *test = arg;
    const bladerf_channel_layout stream_layout =
        test->layout == RX_FAULT_LAYOUT_X2 ? BLADERF_RX_X2 : BLADERF_RX_X1;
    test->stream_status = bladerf_stream(test->stream, stream_layout);
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
            test->stream, test->api_buffer, 0);
        unsetenv("BLADERF_TEST_LIBUSB_RX_API_SUBMIT_ERROR");
    }
    return NULL;
}

static const char *fault_layout_name(enum rx_fault_layout layout)
{
    switch (layout) {
        case RX_FAULT_LAYOUT_RX1:
            return "RX1";
        case RX_FAULT_LAYOUT_RX2:
            return "RX2";
        case RX_FAULT_LAYOUT_X2:
            return "RX_X2";
    }
    return "unknown";
}

int main(void)
{
    struct fault_test test = {0};
    pthread_t thread;
    bool stream_thread_started = false;
    const char *fault_status = getenv("BLADERF_TEST_LIBUSB_RX_STATUS");
    const char *layout = getenv("BLADERF_TEST_RX_LAYOUT");
    test.layout = RX_FAULT_LAYOUT_X2;
    test.transition_channel = BLADERF_CHANNEL_RX(1);
    if (layout != NULL && strcmp(layout, "RX1") == 0) {
        test.layout = RX_FAULT_LAYOUT_RX1;
        test.transition_channel = BLADERF_CHANNEL_RX(0);
    } else if (layout != NULL && strcmp(layout, "RX2") == 0) {
        test.layout = RX_FAULT_LAYOUT_RX2;
        test.transition_channel = BLADERF_CHANNEL_RX(1);
    } else if (layout != NULL && strcmp(layout, "BOTH") != 0) {
        fprintf(stderr, "BLADERF_TEST_RX_LAYOUT must be RX1, RX2, or BOTH\n");
        return 2;
    }
    if (fault_status == NULL) {
        fault_status = "OVERFLOW";
    }
    test.fault_status = fault_status;
    /* The harness receives the selected fault through RX_STATUS for backwards
     * compatibility; disarm all injection variables until certified data is
     * flowing and arm_rx_fault() publishes the requested fault. */
    unsetenv("BLADERF_TEST_LIBUSB_RX_STATUS");
    unsetenv("BLADERF_TEST_LIBUSB_RX_STATUS_AFTER_COMPLETIONS");
    unsetenv("BLADERF_TEST_LIBUSB_RX_EVENT_ERROR");
    unsetenv("BLADERF_TEST_LIBUSB_RX_SUBMIT_ERROR");
    unsetenv("BLADERF_TEST_LIBUSB_RX_API_SUBMIT_ERROR");
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
    } else if (strcmp(fault_status, "EVENT_NODEV") == 0 ||
               strcmp(fault_status, "SUBMIT_NODEV") == 0) {
        test.expected_reason = BLADERF_RF_WITHHELD_DEVICE_LOST;
        test.expected_stream_status = BLADERF_ERR_NODEV;
    } else if (strcmp(fault_status, "SUBMIT_TIMEOUT") == 0) {
        test.expected_reason = BLADERF_RF_WITHHELD_USB_TIMEOUT;
        test.expected_stream_status = BLADERF_ERR_TIMEOUT;
    } else if (strcmp(fault_status, "SUBMIT_IO") == 0) {
        test.expected_reason = BLADERF_RF_WITHHELD_USB_TRANSFER_ERROR;
        test.expected_stream_status = BLADERF_ERR_IO;
    } else if (strcmp(fault_status, "CANCELLED") == 0) {
        test.expected_reason = BLADERF_RF_WITHHELD_USB_TRANSFER_ERROR;
        test.expected_stream_status = BLADERF_ERR_IO;
    } else if (strcmp(fault_status, "UNKNOWN") == 0) {
        test.expected_reason = BLADERF_RF_WITHHELD_USB_TRANSFER_ERROR;
        test.expected_stream_status = BLADERF_ERR_IO;
    } else if (strcmp(fault_status, "SHORT") == 0 ||
               strcmp(fault_status, "SHORT_AFTER_VALID") == 0) {
        test.expected_reason = BLADERF_RF_WITHHELD_SHORT_TRANSFER;
        test.expected_stream_status = 0;
        atomic_store(&test.recoverable_short_mode, true);
        atomic_store(&test.short_after_valid_mode,
                     strcmp(fault_status, "SHORT_AFTER_VALID") == 0);
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
        /* This live test deliberately uses event-driven transfers. The
         * library default one-second bulk transfer timeout would abort the
         * pre-epoch USB queue while the RFIC/FPGA transition is in progress. */
        test.stream->transfer_timeout = 0;
    }
    if (status == 0 && test.layout != RX_FAULT_LAYOUT_RX2) {
        status = bladerf_enable_module(test.dev, BLADERF_CHANNEL_RX(0), true);
    }
    if (status == 0 && test.layout != RX_FAULT_LAYOUT_RX1) {
        status = bladerf_enable_module(test.dev, BLADERF_CHANNEL_RX(1), true);
    }
    if (status != 0) {
        fprintf(stderr, "RX setup failed: %s\n", bladerf_strerror(status));
        goto cleanup;
    }

    /* Establish the event cursor before starting the callback thread. */
    {
        struct bladerf_rf_event events[BLADERF_RF_EVENT_HISTORY_SIZE];
        uint32_t count = 0;
        uint64_t next = 0;
        bool complete = false;
        status = bladerf_rf_events_get_since(
            test.dev, 0, events, BLADERF_RF_EVENT_HISTORY_SIZE,
            &count, &next, &complete);
        if (status != 0 || !complete) {
            fprintf(stderr, "could not initialize RF event cursor\n");
            status = BLADERF_ERR_UNEXPECTED;
            goto cleanup;
        }
        test.event_cursor = next;
    }

    if (pthread_create(&thread, NULL, run_stream, &test) != 0) {
        fprintf(stderr, "could not start RX stream\n");
        status = BLADERF_ERR_UNEXPECTED;
        goto cleanup;
    }
    stream_thread_started = true;

    /* Let the backend queue USB IN transfers before arming the first epoch.
     * This is startup coordination, not an operation completion deadline. */
    const struct timespec backend_startup = {
        .tv_sec = 0, .tv_nsec = 100000000,
    };
    nanosleep(&backend_startup, NULL);

    const struct bladerf_rx_transition_request request = {
        .target_frequency_hz = 1835000000ULL,
        .required_events_mask = BLADERF_RF_REQUIRE_PLL_LOCKED |
                                BLADERF_RF_REQUIRE_ENSM_RX |
                                BLADERF_RF_REQUIRE_EPOCH_VALID,
        .timeout_ms = 0,
        .require_rx_data_valid = true,
        .epoch_settle_samples = 0,
    };
    uint32_t transaction_id = 0;
    struct bladerf_rf_event transition_event;
    status = bladerf_rx_transition_begin(
        test.dev, test.transition_channel, &request, &transaction_id);
    if (status == 0) {
        status = bladerf_rx_transition_wait(
            test.dev, transaction_id, &transition_event, 0);
    }
    if (status != 0) {
        fprintf(stderr, "RX epoch setup failed: %s\n",
                bladerf_strerror(status));
        goto cleanup;
    }
    test.expected_epoch_id = transition_event.epoch_id;
    test.first_valid_timestamp = transition_event.fpga_timestamp;
    atomic_store(&test.expected_epoch_ready, true);

    if (strcmp(fault_status, "API_SUBMIT_IO") == 0) {
        pthread_t submit_thread;
        if (pthread_create(&submit_thread, NULL, run_api_submit, &test) != 0) {
            fprintf(stderr, "could not start API submit fault test\n");
            status = BLADERF_ERR_UNEXPECTED;
            goto cleanup;
        }
        pthread_join(submit_thread, NULL);
    }

    pthread_join(thread, NULL);
    stream_thread_started = false;
    unsetenv("BLADERF_TEST_LIBUSB_RX_STATUS");
    unsetenv("BLADERF_TEST_LIBUSB_RX_STATUS_AFTER_COMPLETIONS");
    unsetenv("BLADERF_TEST_LIBUSB_RX_EVENT_ERROR");
    unsetenv("BLADERF_TEST_LIBUSB_RX_SUBMIT_ERROR");
    unsetenv("BLADERF_TEST_LIBUSB_RX_API_SUBMIT_ERROR");

    const bool event_loop_fault = strncmp(fault_status, "EVENT_", 6) == 0;
    const bool pre_callback_fault = event_loop_fault ||
                                    strcmp(fault_status, "API_SUBMIT_IO") == 0 ||
                                    strcmp(fault_status, "SUBMIT_IO") == 0 ||
                                    strcmp(fault_status, "SUBMIT_NODEV") == 0 ||
                                    strcmp(fault_status, "SUBMIT_TIMEOUT") == 0;
    if (test.stream_status != test.expected_stream_status ||
        atomic_load(&test.pre_fault_data_callbacks) <
            (strcmp(fault_status, "API_SUBMIT_IO") == 0 ? 1u :
             atomic_load(&test.short_after_valid_mode) ? 20u : 3u) ||
        (atomic_load(&test.recoverable_short_mode) &&
         (atomic_load(&test.post_fault_data_callbacks) <
              (atomic_load(&test.short_after_valid_mode) ? 50u : 1u) ||
          (test.layout == RX_FAULT_LAYOUT_RX2
               ? atomic_load(&test.rx2_samples) == 0
               : atomic_load(&test.rx1_samples) == 0) ||
          (test.layout == RX_FAULT_LAYOUT_X2 &&
           atomic_load(&test.rx1_samples) != atomic_load(&test.rx2_samples)) ||
          (test.layout == RX_FAULT_LAYOUT_RX2 &&
           atomic_load(&test.rx2_samples) == 0))) ||
        (strcmp(fault_status, "API_SUBMIT_IO") == 0 &&
         test.api_submit_status != BLADERF_ERR_IO) ||
        atomic_load(&test.withheld_events) != 1 ||
        (pre_callback_fault || atomic_load(&test.recoverable_short_mode)
             ? atomic_load(&test.overrun_events) < 1
             : atomic_load(&test.overrun_events) != 1) ||
        atomic_load(&test.event_callbacks) < 1 ||
        (atomic_load(&test.recoverable_short_mode)
             ? (atomic_load(&test.resumed_events) != 1 ||
                atomic_load(&test.first_valid_events) != 1 ||
                (atomic_load(&test.withheld_timestamp_valid) &&
                 !timestamp_strictly_after(
                     atomic_load(&test.resumed_timestamp),
                     atomic_load(&test.withheld_timestamp))))
             : atomic_load(&test.resumed_events) != 0) ||
        atomic_load(&test.resume_metadata_mismatches) != 0 ||
        atomic_load(&test.data_after_fault) ||
        atomic_load(&test.invalid_meta_buffers) != 0) {
        struct bladerf_rf_event debug_events[BLADERF_RF_EVENT_HISTORY_SIZE];
        uint32_t debug_count = 0;
        uint64_t debug_next = 0;
        bool debug_complete = false;
        if (bladerf_rf_events_get_since(test.dev, 0, debug_events,
                BLADERF_RF_EVENT_HISTORY_SIZE, &debug_count, &debug_next,
                &debug_complete) == 0) {
            for (uint32_t i = 0; i < debug_count; ++i) {
                fprintf(stderr, "event[%u] type=%u reason=0x%x epoch=%u ts=%llu txn=%u\n",
                        i, debug_events[i].event_type, debug_events[i].flags,
                        debug_events[i].epoch_id,
                        (unsigned long long)debug_events[i].fpga_timestamp,
                        debug_events[i].transaction_id);
            }
        }
        fprintf(stderr, "FAIL stream=%s data=%u event_only=%u withheld=%u "
                "overrun=%u rx1_slots=%u rx2_slots=%u invalid_meta=%u "
                "resumed=%u resume_mismatch=%u data_after_fault=%u "
                "first_valid=%u "
                "gap_ts_valid=%u gap_start_ts=%llu resume_ts=%llu "
                "api_submit=%s expected_reason=0x%x\n",
                bladerf_strerror(test.stream_status),
                atomic_load(&test.data_callbacks),
                atomic_load(&test.event_callbacks),
                atomic_load(&test.withheld_events),
                atomic_load(&test.overrun_events),
                atomic_load(&test.rx1_samples),
                atomic_load(&test.rx2_samples),
                atomic_load(&test.invalid_meta_buffers),
                atomic_load(&test.resumed_events),
                atomic_load(&test.resume_metadata_mismatches),
                atomic_load(&test.data_after_fault),
                atomic_load(&test.first_valid_events),
                atomic_load(&test.withheld_timestamp_valid),
                (unsigned long long)atomic_load(&test.withheld_timestamp),
                (unsigned long long)atomic_load(&test.resumed_timestamp),
                bladerf_strerror(test.api_submit_status), test.expected_reason);
        status = BLADERF_ERR_UNEXPECTED;
    } else if (atomic_load(&test.recoverable_short_mode)) {
        printf("PASS libusb %s %s callback: valid_IQ=%u "
               "first_valid_events=%u resume_events=%u "
               "gap_ts_valid=%u gap_start_ts=%llu resume_ts=%llu "
               "rx1_slots=%u rx2_slots=%u invalid_meta=%u event_only=%u "
               "withheld=%u overrun=%u stream=%s\n",
               fault_layout_name(test.layout),
               fault_status,
               atomic_load(&test.data_callbacks),
               atomic_load(&test.first_valid_events),
               atomic_load(&test.resumed_events),
               atomic_load(&test.withheld_timestamp_valid),
               (unsigned long long)atomic_load(&test.withheld_timestamp),
               (unsigned long long)atomic_load(&test.resumed_timestamp),
               atomic_load(&test.rx1_samples),
               atomic_load(&test.rx2_samples),
               atomic_load(&test.invalid_meta_buffers),
               atomic_load(&test.event_callbacks),
               atomic_load(&test.withheld_events),
               atomic_load(&test.overrun_events),
               bladerf_strerror(test.stream_status));
        status = 0;
    } else {
        printf("PASS libusb %s %s callback: data=%u resume_events=%u rx1_slots=%u "
               "rx2_slots=%u invalid_meta=%u event_only=%u withheld=%u "
               "overrun=%u post_fault_IQ=0 stream=%s\n",
               fault_layout_name(test.layout),
               fault_status,
               atomic_load(&test.data_callbacks),
               atomic_load(&test.resumed_events),
               atomic_load(&test.rx1_samples),
               atomic_load(&test.rx2_samples),
               atomic_load(&test.invalid_meta_buffers),
               atomic_load(&test.event_callbacks),
               atomic_load(&test.withheld_events),
               atomic_load(&test.overrun_events),
               bladerf_strerror(test.stream_status));
        status = 0;
    }

cleanup:
    if (stream_thread_started) {
        (void)bladerf_submit_stream_buffer(
            test.stream, BLADERF_STREAM_SHUTDOWN, 0);
        pthread_join(thread, NULL);
        stream_thread_started = false;
    }
    if (test.dev != NULL) {
        unsetenv("BLADERF_TEST_LIBUSB_RX_STATUS");
        unsetenv("BLADERF_TEST_LIBUSB_RX_STATUS_AFTER_COMPLETIONS");
        unsetenv("BLADERF_TEST_LIBUSB_RX_EVENT_ERROR");
        unsetenv("BLADERF_TEST_LIBUSB_RX_SUBMIT_ERROR");
        unsetenv("BLADERF_TEST_LIBUSB_RX_API_SUBMIT_ERROR");
        if (test.layout != RX_FAULT_LAYOUT_RX2) {
            bladerf_enable_module(test.dev, BLADERF_CHANNEL_RX(0), false);
        }
        if (test.layout != RX_FAULT_LAYOUT_RX1) {
            bladerf_enable_module(test.dev, BLADERF_CHANNEL_RX(1), false);
        }
        if (test.stream != NULL) {
            bladerf_deinit_stream(test.stream);
        }
        bladerf_close(test.dev);
    }
    return status == 0 ? 0 : 1;
}
