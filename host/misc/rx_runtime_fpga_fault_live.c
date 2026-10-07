#define _DEFAULT_SOURCE

/* Test-build xA4 check: the board control-plane monitor must notice a sticky
 * FPGA RX fault after epoch certification, revoke it, and publish the source
 * status without waiting for a libusb completion callback. Build with
 * ENABLE_TEST_RX_TRANSITION_STALL_INJECTION=ON and run with
 * BLADERF_TEST_RX_TRANSITION_STALL=RUNTIME_FPGA_FAULT, or use the
 * RUNTIME_FPGA_STATUS_READ_FAILURE / RUNTIME_FPGA_STATUS_VERSION, or
 * RUNTIME_RFIC_* values to verify runtime control-plane loss handling. */
#include <libbladeRF.h>

#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

#define RX_FAULT_STATUS_BIT (1u << 14)
#define BLOCKING_READ_SAMPLES (4u * 1024u * 1024u)

struct blocking_reader {
    struct bladerf *dev;
    int16_t *samples;
    unsigned int sample_count;
    struct bladerf_metadata metadata;
    int status;
    atomic_bool started;
    atomic_bool finished;
};

static void *blocking_sync_reader(void *arg)
{
    struct blocking_reader *reader = arg;
    struct timespec begin, end;
    clock_gettime(CLOCK_MONOTONIC, &begin);
    atomic_store(&reader->started, true);
    reader->status = bladerf_sync_rx(reader->dev, reader->samples,
                                     reader->sample_count,
                                     &reader->metadata, 5000);
    clock_gettime(CLOCK_MONOTONIC, &end);
    fprintf(stderr, "blocking sync reader returned after %.3f s status=%d "
            "count=%u\n",
            (end.tv_sec - begin.tv_sec) +
                (end.tv_nsec - begin.tv_nsec) / 1e9,
            reader->status, reader->metadata.actual_count);
    atomic_store(&reader->finished, true);
    return NULL;
}

int main(int argc, char **argv)
{
    struct bladerf *dev = NULL;
    int16_t samples[8192 * 4];
    bladerf_channel transition_channel = BLADERF_CHANNEL_RX(0);
    bladerf_channel_layout layout = BLADERF_RX_X1;
    bool enable_rx1 = true;
    bool enable_rx2 = false;
    bool reader_started = false;
    pthread_t reader_thread;
    struct blocking_reader reader = {0};
    const char *requested_fault_mode =
        getenv("BLADERF_TEST_RX_TRANSITION_STALL");
    char fault_mode_storage[64];
    const char *fault_mode = fault_mode_storage;
    uint32_t expected_reason = BLADERF_RF_INVALIDATE_FPGA_RX_FAULT;
    int expected_error = 0;
    uint32_t expected_rfic_mask = 0;
    uint32_t expected_rfic_value = 0;
    uint32_t transaction_id = 0;
    int status;

    if (argc > 1 && strcmp(argv[1], "RX2") == 0) {
        transition_channel = BLADERF_CHANNEL_RX(1);
        enable_rx1 = false;
        enable_rx2 = true;
    } else if (argc > 1 && strcmp(argv[1], "RX_X2") == 0) {
        transition_channel = BLADERF_CHANNEL_RX(1);
        layout = BLADERF_RX_X2;
        enable_rx2 = true;
    } else if (argc > 1 && strcmp(argv[1], "RX1") != 0) {
        fprintf(stderr, "usage: %s [RX1|RX2|RX_X2]\n", argv[0]);
        return 2;
    }
    if (requested_fault_mode == NULL ||
        strlen(requested_fault_mode) >= sizeof(fault_mode_storage)) {
        fprintf(stderr, "set one runtime monitor injection mode in the env\n");
        return 2;
    }
    strcpy(fault_mode_storage, requested_fault_mode);
    if (strcmp(fault_mode, "RUNTIME_FPGA_STATUS_READ_FAILURE") == 0 ||
        strcmp(fault_mode, "RUNTIME_FPGA_STATUS_VERSION") == 0) {
        expected_reason = BLADERF_RF_INVALIDATE_FPGA_STATUS_UNAVAILABLE;
        expected_error = strcmp(fault_mode,
            "RUNTIME_FPGA_STATUS_READ_FAILURE") == 0 ? BLADERF_ERR_IO :
            BLADERF_ERR_UNEXPECTED;
    } else if (strcmp(fault_mode, "RUNTIME_RFIC_STATUS_READ_FAILURE") == 0) {
        expected_reason = BLADERF_RF_INVALIDATE_RFIC_STATUS_UNAVAILABLE;
        expected_error = BLADERF_ERR_UNEXPECTED;
    } else if (strcmp(fault_mode, "RUNTIME_RFIC_PLL_UNLOCKED") == 0) {
        expected_reason = BLADERF_RF_INVALIDATE_RFIC_PLL_UNLOCKED;
        expected_rfic_mask = 0x02;
    } else if (strcmp(fault_mode, "RUNTIME_RFIC_ENSM_NOT_RX") == 0) {
        expected_reason = BLADERF_RF_INVALIDATE_RFIC_ENSM_NOT_RX;
        expected_rfic_mask = 0x0f;
        expected_rfic_value = 0x05;
    } else if (strcmp(fault_mode, "RUNTIME_RFIC_BBPLL_UNLOCKED") == 0) {
        expected_reason = BLADERF_RF_INVALIDATE_RFIC_BBPLL_UNLOCKED;
        expected_rfic_mask = 0x80;
    } else if (strcmp(fault_mode, "RUNTIME_FPGA_FAULT") != 0) {
        fprintf(stderr, "unknown monitor injection mode: %s\n", fault_mode);
        return 2;
    }
    if (unsetenv("BLADERF_TEST_RX_TRANSITION_STALL") != 0) {
        fprintf(stderr, "set one runtime monitor injection mode in the env\n");
        return 2;
    }

    status = bladerf_open(&dev, NULL);
    if (status != 0) goto cleanup;
    bladerf_log_set_verbosity(BLADERF_LOG_LEVEL_WARNING);

    status = bladerf_set_sample_rate(dev, transition_channel, 1000000, NULL);
    if (status != 0) goto cleanup;
    status = bladerf_set_bandwidth(dev, transition_channel, 1500000, NULL);
    if (status != 0) goto cleanup;
    status = bladerf_sync_config(dev, layout, BLADERF_FORMAT_SC16_Q11_META,
                                 16, 8192, 8, 1000);
    if (status != 0) goto cleanup;
    if (enable_rx1) {
        status = bladerf_enable_module(dev, BLADERF_CHANNEL_RX(0), true);
        if (status != 0) goto cleanup;
    }
    if (enable_rx2) {
        status = bladerf_enable_module(dev, BLADERF_CHANNEL_RX(1), true);
        if (status != 0) goto cleanup;
    }

    struct bladerf_rx_transition_request request = {
        .target_frequency_hz = 1835000000ULL,
        .required_events_mask = BLADERF_RF_REQUIRE_PLL_LOCKED,
        .timeout_ms = 2000,
        .require_rx_data_valid = true,
        .epoch_settle_samples = 0,
    };
    struct bladerf_rf_event transition_event = {0};
    struct bladerf_rf_event events[BLADERF_RF_EVENT_HISTORY_SIZE];
    uint32_t event_count = 0;
    uint64_t cursor = 0;
    bool complete = false;
    status = bladerf_rx_transition_begin(dev, transition_channel, &request,
                                         &transaction_id);
    if (status != 0) goto cleanup;
    status = bladerf_rx_transition_wait(dev, transaction_id,
                                        &transition_event, 2000);
    if (status != 0 ||
        transition_event.event_type != BLADERF_RF_EVT_RX_EPOCH_VALID) {
        fprintf(stderr, "pre-fault transition failed: %s event=%u\n",
                bladerf_strerror(status), transition_event.event_type);
        status = BLADERF_ERR_UNEXPECTED;
        goto cleanup;
    }

    /* Drain startup buffering once so the long read below is demonstrably
     * consuming a certified stream instead of only rejecting the unprimed
     * pre-boundary queue. */
    struct bladerf_metadata prime_metadata = {
        .flags = BLADERF_META_FLAG_RX_NOW
    };
    for (unsigned attempt = 0; attempt < 40; ++attempt) {
        status = bladerf_sync_rx(dev, samples, 8192, &prime_metadata, 1000);
        if (status != BLADERF_ERR_WOULD_BLOCK) break;
        prime_metadata.flags = BLADERF_META_FLAG_RX_NOW;
        usleep(25000);
    }
    if (status != 0 || prime_metadata.actual_count == 0 ||
        !prime_metadata.rx_epoch_id_valid ||
        prime_metadata.rx_epoch_id != transition_event.epoch_id) {
        fprintf(stderr, "pre-fault certified read failed: %s count=%u\n",
                bladerf_strerror(status), prime_metadata.actual_count);
        status = BLADERF_ERR_UNEXPECTED;
        goto cleanup;
    }
    status = bladerf_rf_events_get_since(
        dev, 0, events, BLADERF_RF_EVENT_HISTORY_SIZE, &event_count,
        &cursor, &complete);
    if (status != 0 || !complete) {
        status = status != 0 ? status : BLADERF_ERR_UNEXPECTED;
        goto cleanup;
    }

    /* Keep a long sync read inside sync_rx while the background monitor
     * observes the injected fault. The event must enter native history
     * before this read releases sync->lock (up to its 5s timeout). */
    reader.dev = dev;
    reader.sample_count = BLOCKING_READ_SAMPLES;
    reader.samples = calloc((size_t)BLOCKING_READ_SAMPLES *
        (layout == BLADERF_RX_X2 ? 4u : 2u), sizeof(*reader.samples));
    if (reader.samples == NULL) {
        status = BLADERF_ERR_MEM;
        goto cleanup;
    }
    reader.metadata.flags = BLADERF_META_FLAG_RX_NOW;
    atomic_init(&reader.started, false);
    atomic_init(&reader.finished, false);
    status = pthread_create(&reader_thread, NULL, blocking_sync_reader,
                            &reader);
    if (status != 0) {
        status = BLADERF_ERR_UNEXPECTED;
        goto cleanup;
    }
    reader_started = true;
    for (unsigned attempt = 0; attempt < 100 &&
         !atomic_load(&reader.started); ++attempt) {
        usleep(1000);
    }
    if (!atomic_load(&reader.started)) {
        status = BLADERF_ERR_TIMEOUT;
        goto cleanup;
    }
    /* Let sync_rx enter its long read before enabling the monitor fault. */
    usleep(20000);
    if (setenv("BLADERF_TEST_RX_TRANSITION_STALL", fault_mode, 1) != 0) {
        status = BLADERF_ERR_UNEXPECTED;
        goto cleanup;
    }

    bool saw_fault_invalidation = false;
    for (unsigned attempt = 0; attempt < 3000 && !saw_fault_invalidation;
         ++attempt) {
        usleep(1000);
        uint64_t next_cursor = cursor;
        status = bladerf_rf_events_get_since(
            dev, cursor, events, BLADERF_RF_EVENT_HISTORY_SIZE,
            &event_count, &next_cursor, &complete);
        if (status != 0 || !complete) {
            status = status != 0 ? status : BLADERF_ERR_UNEXPECTED;
            goto cleanup;
        }
        cursor = next_cursor;
        for (uint32_t i = 0; i < event_count; ++i) {
            if (events[i].event_type == BLADERF_RF_EVT_RX_DATA_INVALIDATED &&
                events[i].flags == expected_reason &&
                (expected_reason != BLADERF_RF_INVALIDATE_FPGA_RX_FAULT ||
                 (events[i].rfic_status & RX_FAULT_STATUS_BIT) != 0) &&
                (events[i].rfic_status & expected_rfic_mask) ==
                    expected_rfic_value &&
                (expected_error == 0 || events[i].error_code == expected_error) &&
                events[i].epoch_id == transition_event.epoch_id) {
                saw_fault_invalidation = true;
                break;
            }
        }
    }
    if (!saw_fault_invalidation) {
        fprintf(stderr, "runtime FPGA fault invalidation event not observed\n");
        status = BLADERF_ERR_UNEXPECTED;
        goto cleanup;
    }
    unsetenv("BLADERF_TEST_RX_TRANSITION_STALL");
    if (atomic_load(&reader.finished)) {
        fprintf(stderr, "fault event was delayed until sync reader exited\n");
        status = BLADERF_ERR_UNEXPECTED;
        goto cleanup;
    }
    pthread_join(reader_thread, NULL);
    reader_started = false;

    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i) {
        samples[i] = 0x5555;
    }
    struct bladerf_metadata metadata = { .flags = BLADERF_META_FLAG_RX_NOW };
    int rx_status = bladerf_sync_rx(dev, samples, 8192, &metadata, 100);
    if (rx_status == 0 || metadata.actual_count != 0) {
        fprintf(stderr, "runtime FPGA fault leaked IQ: status=%s count=%u\n",
                bladerf_strerror(rx_status), metadata.actual_count);
        status = BLADERF_ERR_UNEXPECTED;
        goto cleanup;
    }
    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i) {
        if (samples[i] != 0x5555) {
            fprintf(stderr, "runtime FPGA fault modified sample buffer\n");
            status = BLADERF_ERR_UNEXPECTED;
            goto cleanup;
        }
    }

    /* Prove recovery requires a fresh explicit epoch after the injected
     * fault/status failure has been removed. */
    if (setenv("BLADERF_TEST_RX_TRANSITION_STALL", "", 1) != 0) {
        status = BLADERF_ERR_UNEXPECTED;
        goto cleanup;
    }
    request.target_frequency_hz += 100000;
    status = bladerf_rx_transition_begin(dev, transition_channel, &request,
                                         &transaction_id);
    if (status != 0) goto cleanup;
    struct bladerf_rf_event recovery_event = {0};
    status = bladerf_rx_transition_wait(dev, transaction_id, &recovery_event,
                                        2000);
    if (status != 0 ||
        recovery_event.event_type != BLADERF_RF_EVT_RX_EPOCH_VALID ||
        recovery_event.epoch_id == transition_event.epoch_id) {
        fprintf(stderr, "explicit recovery transition failed: %s event=%u\n",
                bladerf_strerror(status), recovery_event.event_type);
        status = BLADERF_ERR_UNEXPECTED;
        goto cleanup;
    }
    struct bladerf_metadata recovery_metadata = {
        .flags = BLADERF_META_FLAG_RX_NOW
    };
    /* The abort can leave one explicit overrun notification queued for the
     * first read after recovery. Consume that event-only read, then require
     * the following call to return the new epoch's IQ. */
    status = bladerf_sync_rx(dev, samples, 8192, &recovery_metadata, 1000);
    if (status == BLADERF_ERR_WOULD_BLOCK &&
        recovery_metadata.actual_count == 0) {
        recovery_metadata.flags = BLADERF_META_FLAG_RX_NOW;
        status = bladerf_sync_rx(dev, samples, 8192, &recovery_metadata, 1000);
    }
    if (status != 0 || recovery_metadata.actual_count < 8192 - 16 ||
        !recovery_metadata.rx_epoch_id_valid ||
        recovery_metadata.rx_epoch_id != recovery_event.epoch_id) {
        fprintf(stderr, "explicit recovery invalid IQ: %s count=%u epoch=%u/%u "
                "epoch_valid=%u meta_status=0x%x\n",
                bladerf_strerror(status), recovery_metadata.actual_count,
                recovery_metadata.rx_epoch_id, recovery_event.epoch_id,
                recovery_metadata.rx_epoch_id_valid, recovery_metadata.status);
        status = BLADERF_ERR_UNEXPECTED;
        goto cleanup;
    }

    printf("runtime FPGA RX fault monitor: PASS layout=%s epoch=%u "
           "reason=0x%x iq_count=0 recovered_epoch=%u recovered_iq=%u\n",
           layout == BLADERF_RX_X2 ? "RX_X2" :
           (transition_channel == BLADERF_CHANNEL_RX(1) ? "RX2" : "RX1"),
           transition_event.epoch_id, expected_reason, recovery_event.epoch_id,
           recovery_metadata.actual_count);
    status = 0;

cleanup:
    if (reader_started) {
        pthread_join(reader_thread, NULL);
    }
    free(reader.samples);
    if (dev != NULL) {
        if (enable_rx1) {
            bladerf_enable_module(dev, BLADERF_CHANNEL_RX(0), false);
        }
        if (enable_rx2) {
            bladerf_enable_module(dev, BLADERF_CHANNEL_RX(1), false);
        }
        bladerf_close(dev);
    }
    if (status != 0) {
        fprintf(stderr, "runtime FPGA RX fault test failed: %s\n",
                bladerf_strerror(status));
    }
    return status == 0 ? 0 : 1;
}
