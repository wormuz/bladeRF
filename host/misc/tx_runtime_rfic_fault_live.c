#define _DEFAULT_SOURCE

/* Test-build xA4 qualification for TX RFIC monitor invalidation. Build with
 * ENABLE_TEST_RX_TRANSITION_STALL_INJECTION=ON and set
 * BLADERF_TEST_RX_TRANSITION_STALL to one of:
 *   RUNTIME_TX_RFIC_STATUS_READ_FAILURE
 *   RUNTIME_TX_RFPLL_UNLOCKED
 *   RUNTIME_TX_ENSM_NOT_TX
 *   RUNTIME_TX_BBPLL_UNLOCKED
 *   RUNTIME_TX_CALIBRATION_ACTIVE
 *   RUNTIME_REFERENCE_PLL_UNLOCKED
 *   RUNTIME_TX_LINK_FAULT_NO_RFIC_READ
 *   RUNTIME_TX_RETUNE_FAILURE
 *   RUNTIME_TX_RETUNE_FAILURE_TX_DISABLED
 *   RUNTIME_TX_RFIC_REGISTER_WRITE
 *   RUNTIME_TX_GAIN_SETTER
 *   RUNTIME_TX_QUEUE_CLEAR_FAILURE
 *   RUNTIME_TX_GATE_STATUS_READ_FAILURE
 *   RUNTIME_TX_GATE_RELEASE_FAILURE
 *   RUNTIME_TX_GATE_ABORT_FAILURE
 *   RUNTIME_TX_TRANSPORT_FAILURE_DURING_RELEASE
 *
 * The test verifies durable event classification and that sync TX refuses
 * samples after the monitor or a legacy RF configuration setter revokes
 * validity. */
#include <libbladeRF.h>

#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define TX_SAMPLES 4096u
#define TX_FEEDER_TIMEOUT_MS 0u
#define RX_OBSERVER_SAMPLES 4096u

struct rx_transition_waiter {
    struct bladerf *dev;
    uint32_t transaction_id;
    struct bladerf_rf_event final_event;
    int status;
};

struct rx_observer_reader {
    struct bladerf *dev;
    atomic_bool stop;
    atomic_int terminal_status;
    int16_t samples[RX_OBSERVER_SAMPLES * 4];
};

static void *wait_for_rx_transition(void *arg)
{
    struct rx_transition_waiter *waiter = arg;
    waiter->status = bladerf_rx_transition_wait(
        waiter->dev, waiter->transaction_id, &waiter->final_event, 0);
    return NULL;
}

static void *drain_rx_observer(void *arg)
{
    struct rx_observer_reader *reader = arg;
    while (!atomic_load(&reader->stop)) {
        struct bladerf_metadata metadata = {
            .flags = BLADERF_META_FLAG_RX_NOW
        };
        const int status = bladerf_sync_rx(
            reader->dev, reader->samples, RX_OBSERVER_SAMPLES * 2,
            &metadata, TX_FEEDER_TIMEOUT_MS);
        if (status == 0 || status == BLADERF_ERR_TIMEOUT ||
            status == BLADERF_ERR_WOULD_BLOCK) {
            continue;
        }
        atomic_store(&reader->terminal_status, status);
        break;
    }
    return NULL;
}

static int report_rx_prepare_error(const char *operation, int status)
{
    if (status != 0) {
        fprintf(stderr, "RX_X2 observer %s failed: %s (%d)\n", operation,
                bladerf_strerror(status), status);
    }
    return status;
}

static int prepare_rx_x2_observer(struct bladerf *dev)
{
    const bladerf_channel rx0 = BLADERF_CHANNEL_RX(0);
    const bladerf_channel rx1 = BLADERF_CHANNEL_RX(1);
    int status;
    unsigned int actual_rate = 0, actual_bandwidth = 0;

    status = bladerf_set_sample_rate(dev, rx0, 1000000, &actual_rate);
    status = report_rx_prepare_error("RX0 sample rate", status);
    if (status == 0)
        status = bladerf_set_bandwidth(dev, rx0, 1500000, &actual_bandwidth);
    status = report_rx_prepare_error("RX0 bandwidth", status);
    if (status == 0)
        status = bladerf_set_sample_rate(dev, rx1, 1000000, &actual_rate);
    status = report_rx_prepare_error("RX1 sample rate", status);
    if (status == 0)
        status = bladerf_set_bandwidth(dev, rx1, 1500000, &actual_bandwidth);
    status = report_rx_prepare_error("RX1 bandwidth", status);
    if (status == 0)
        status = bladerf_sync_config(dev, BLADERF_RX_X2,
                                     BLADERF_FORMAT_SC16_Q11_META,
                                     8, RX_OBSERVER_SAMPLES * 2, 4, 0);
    status = report_rx_prepare_error("sync config", status);
    if (status == 0)
        status = bladerf_enable_module(dev, rx0, true);
    status = report_rx_prepare_error("enable RX0", status);
    if (status == 0)
        status = bladerf_enable_module(dev, rx1, true);
    status = report_rx_prepare_error("enable RX1", status);
    return status;
}

static bool mode_needs_tx_timestamp(const char *mode)
{
    return strcmp(mode, "RUNTIME_TX_RETUNE_FAILURE") == 0 ||
           strcmp(mode, "RUNTIME_TX_RETUNE_FAILURE_TX_DISABLED") == 0 ||
           strcmp(mode, "RUNTIME_TX_QUEUE_CLEAR_FAILURE") == 0 ||
           strcmp(mode, "RUNTIME_TX_GATE_STATUS_READ_FAILURE") == 0 ||
           strcmp(mode, "RUNTIME_TX_GATE_RELEASE_FAILURE") == 0 ||
           strcmp(mode, "RUNTIME_TX_GATE_ABORT_FAILURE") == 0 ||
           strcmp(mode, "RUNTIME_TX_TRANSPORT_FAILURE_DURING_RELEASE") == 0;
}

/* TX timestamp reads require an active RX_X2 host-data epoch on this image.
 * Keep the TX-only retune fault test's prerequisite explicit and event driven. */
static int qualify_rx_x2_observer(struct bladerf *dev,
                                  struct rx_observer_reader *reader,
                                  pthread_t *reader_thread)
{
    const bladerf_channel rx0 = BLADERF_CHANNEL_RX(0);
    struct bladerf_rx_transition_request request = {
        .target_frequency_hz = 915000000,
        .required_events_mask = BLADERF_RF_REQUIRE_RX_X2_HOST_DATA,
        .require_rx_data_valid = true,
        .timeout_ms = 0,
    };
    struct rx_transition_waiter waiter = { .dev = dev };
    struct bladerf_metadata metadata = { .flags = BLADERF_META_FLAG_RX_NOW };
    int16_t samples[RX_OBSERVER_SAMPLES * 4] = {0};
    pthread_t wait_thread;
    uint32_t transaction_id = 0;
    int status;

    status = bladerf_rx_transition_begin(dev, rx0, &request,
                                         &transaction_id);
    if (status != 0) {
        fprintf(stderr, "begin RX_X2 transition: %s (%d)\n",
                bladerf_strerror(status), status);
        return status;
    }

    waiter.transaction_id = transaction_id;
    status = pthread_create(&wait_thread, NULL, wait_for_rx_transition,
                            &waiter);
    if (status != 0) {
        fprintf(stderr, "start RX transition waiter failed (%d)\n", status);
        return BLADERF_ERR_UNEXPECTED;
    }

    for (;;) {
        metadata = (struct bladerf_metadata){
            .flags = BLADERF_META_FLAG_RX_NOW
        };
        status = bladerf_sync_rx(dev, samples, RX_OBSERVER_SAMPLES * 2,
                                 &metadata, 0);
        if (status == BLADERF_ERR_TIMEOUT ||
            status == BLADERF_ERR_WOULD_BLOCK) {
            continue;
        }
        break;
    }
    (void)pthread_join(wait_thread, NULL);
    if (status == 0)
        status = waiter.status;
    if (status == 0 &&
        (waiter.final_event.event_type !=
             BLADERF_RF_EVT_RX_FIRST_VALID_HOST_DATA ||
         !(waiter.final_event.flags & BLADERF_RF_EVENT_F_RX_X2_LAYOUT))) {
        status = BLADERF_ERR_UNEXPECTED;
    }
    if (status == 0) {
        reader->dev = dev;
        atomic_init(&reader->stop, false);
        atomic_init(&reader->terminal_status, 0);
        status = pthread_create(reader_thread, NULL, drain_rx_observer,
                                reader);
        if (status != 0) {
            status = BLADERF_ERR_UNEXPECTED;
        }
    }
    if (status != 0) {
        fprintf(stderr, "read first RX_X2 host block: %s (%d)\n",
                bladerf_strerror(status), status);
    }
    return status;
}

struct tx_feeder {
    struct bladerf *dev;
    int16_t samples[TX_SAMPLES * 2];
    bool metadata_format;
    atomic_bool stop;
    atomic_int terminal_status;
    atomic_uint accepted_buffers;
};

static int submit_tx_feeder_buffer(struct tx_feeder *feeder)
{
    struct bladerf_metadata metadata = {0};
    struct bladerf_metadata *metadata_ptr = NULL;
    if (feeder->metadata_format) {
        metadata.flags = BLADERF_META_FLAG_TX_BURST_START |
                         BLADERF_META_FLAG_TX_NOW |
                         BLADERF_META_FLAG_TX_BURST_END;
        metadata_ptr = &metadata;
    }
    return bladerf_sync_tx(feeder->dev, feeder->samples, TX_SAMPLES,
                           metadata_ptr, TX_FEEDER_TIMEOUT_MS);
}

static int submit_tx_scheduled_hold(struct tx_feeder *feeder,
                                    bladerf_timestamp timestamp)
{
    struct bladerf_metadata metadata = {0};
    metadata.flags = BLADERF_META_FLAG_TX_BURST_START |
                     BLADERF_META_FLAG_TX_BURST_END;
    metadata.timestamp = timestamp;
    const int status = bladerf_sync_tx(feeder->dev, feeder->samples,
                                       TX_SAMPLES, &metadata,
                           TX_FEEDER_TIMEOUT_MS);
    if (status != 0) {
        fprintf(stderr, "scheduled TX burst submit failed at ts=%" PRIu64
                        " samples=%u: %s (%d)\n",
                timestamp, TX_SAMPLES, bladerf_strerror(status), status);
    }
    return status;
}

static void *feed_tx_until_fault(void *arg)
{
    struct tx_feeder *feeder = arg;
    while (!atomic_load(&feeder->stop)) {
        const int status = submit_tx_feeder_buffer(feeder);
        if (status == 0) {
            atomic_fetch_add(&feeder->accepted_buffers, 1);
        }
        if (status == 0 || status == BLADERF_ERR_TIMEOUT ||
            status == BLADERF_ERR_WOULD_BLOCK) {
            continue;
        }
        atomic_store(&feeder->terminal_status, status);
        break;
    }
    return NULL;
}

static int start_tx_feeder(struct tx_feeder *feeder,
                           pthread_t *thread,
                           bool *started)
{
    atomic_init(&feeder->stop, false);
    atomic_init(&feeder->terminal_status, 0);
    atomic_init(&feeder->accepted_buffers, 0);
    int status = pthread_create(thread, NULL, feed_tx_until_fault, feeder);
    if (status != 0) {
        return BLADERF_ERR_UNEXPECTED;
    }
    *started = true;

    while (atomic_load(&feeder->accepted_buffers) == 0) {
        status = atomic_load(&feeder->terminal_status);
        if (status != 0) {
            return status;
        }
        const struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000};
        nanosleep(&pause, NULL);
    }
    return 0;
}

static int wait_for_event(struct bladerf *dev,
                          struct bladerf_rf_event *events,
                          uint64_t *cursor,
                          bladerf_rf_event_type expected)
{
    const bool trace_events = getenv("BLADERF_TX_RETUNE_TRACE_EVENTS") != NULL;
    for (;;) {
        const uint64_t starting_cursor = *cursor;
        uint64_t next = *cursor;
        uint32_t count = 0;
        bool history_complete = false;
        int status = bladerf_rf_events_get_since(
            dev, *cursor, events, BLADERF_RF_EVENT_HISTORY_SIZE,
            &count, &next, &history_complete);
        if (status != 0 || !history_complete) {
            return status != 0 ? status : BLADERF_ERR_UNEXPECTED;
        }
        for (uint32_t i = 0; i < count; ++i) {
            if (trace_events) {
                fprintf(stderr, "observed event type=%u txn=%u epoch=%u "
                                "error=%d status=0x%08x flags=0x%x "
                                "ts=%" PRIu64 " requested=%" PRIu64 " "
                                "readback=%" PRIu64 "\n",
                        events[i].event_type, events[i].transaction_id,
                        events[i].epoch_id, events[i].error_code,
                        events[i].rfic_status, events[i].flags,
                        events[i].fpga_timestamp,
                        events[i].requested_rx_lo_hz,
                        events[i].readback_rx_lo_hz);
            }
            if (events[i].event_type == expected) {
                *cursor = starting_cursor + i + 1u;
                return 0;
            }
            if (events[i].event_type == BLADERF_RF_EVT_TX_RETUNE_FAILED ||
                events[i].event_type ==
                    BLADERF_RF_EVT_TX_RETUNE_COMPLETION_LOST) {
                if (expected == BLADERF_RF_EVT_TX_STATUS_UNAVAILABLE &&
                    events[i].event_type ==
                        BLADERF_RF_EVT_TX_RETUNE_COMPLETION_LOST) {
                    continue;
                }
                const bool stage_valid =
                    (events[i].flags &
                     BLADERF_RF_EVENT_F_NIOS_FAILURE_STAGE_VALID) != 0;
                fprintf(stderr, "TX retune ended before expected event %u "
                                "with event %u: error=%d "
                                "stage=%s%u rfic_status=0x%08x flags=0x%x "
                                "ts=%" PRIu64 " requested=%" PRIu64 " "
                                "readback=%" PRIu64 "\n",
                        expected, events[i].event_type,
                        events[i].error_code,
                        stage_valid ? "" : "unavailable/",
                        stage_valid ? events[i].rfic_status : 0,
                        events[i].rfic_status, events[i].flags,
                        events[i].fpga_timestamp,
                        events[i].requested_rx_lo_hz,
                        events[i].readback_rx_lo_hz);
                for (uint32_t j = 0; j < count; ++j) {
                    fprintf(stderr, "  event[%u] type=%u txn=%u epoch=%u "
                                    "error=%d status=0x%08x flags=0x%x "
                                    "ts=%" PRIu64 " requested=%" PRIu64 " "
                                    "readback=%" PRIu64 "\n",
                            j, events[j].event_type,
                            events[j].transaction_id, events[j].epoch_id,
                            events[j].error_code, events[j].rfic_status,
                            events[j].flags, events[j].fpga_timestamp,
                            events[j].requested_rx_lo_hz,
                            events[j].readback_rx_lo_hz);
                }
                return BLADERF_ERR_FPGA_OP;
            }
            if ((expected == BLADERF_RF_EVT_TX_RFIC_STATE_VALID ||
                 expected == BLADERF_RF_EVT_TX_RETUNE_VALID) &&
                (events[i].event_type == BLADERF_RF_EVT_TX_STATUS_UNAVAILABLE ||
                 events[i].event_type == BLADERF_RF_EVT_TX_LINK_FAULT ||
                 events[i].event_type == BLADERF_RF_EVT_TX_EPOCH_INVALID ||
                 events[i].event_type == BLADERF_RF_EVT_TX_TRANSPORT_ERROR)) {
                fprintf(stderr, "TX recovery ended before expected event %u "
                                "with terminal event %u: error=%d "
                                "status=0x%08x flags=0x%x\n",
                        (unsigned)expected,
                        (unsigned)events[i].event_type,
                        events[i].error_code, events[i].rfic_status,
                        events[i].flags);
                return BLADERF_ERR_FPGA_OP;
            }
        }
        *cursor = next;
        const struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000};
        nanosleep(&pause, NULL);
    }
}

/* A TX retune can terminate either with a precise RFIC failure or with lost
 * completion/status evidence. Both must fail closed; do not wait only for
 * TX_RETUNE_FAILED after the module has been disabled. */
static int wait_for_tx_retune_terminal_event(
    struct bladerf *dev, struct bladerf_rf_event *events, uint64_t *cursor,
    bladerf_rf_event_type *observed)
{
    for (;;) {
        const uint64_t starting_cursor = *cursor;
        uint64_t next = *cursor;
        uint32_t count = 0;
        bool complete = false;
        int status = bladerf_rf_events_get_since(
            dev, *cursor, events, BLADERF_RF_EVENT_HISTORY_SIZE,
            &count, &next, &complete);
        if (status != 0 || !complete) {
            return status != 0 ? status : BLADERF_ERR_UNEXPECTED;
        }
        for (uint32_t i = 0; i < count; ++i) {
            const bladerf_rf_event_type type = events[i].event_type;
            if (type == BLADERF_RF_EVT_TX_RETUNE_FAILED ||
                type == BLADERF_RF_EVT_TX_RETUNE_COMPLETION_LOST ||
                type == BLADERF_RF_EVT_TX_STATUS_UNAVAILABLE) {
                *cursor = starting_cursor + i + 1u;
                *observed = type;
                return 0;
            }
            if (type == BLADERF_RF_EVT_TX_RETUNE_VALID ||
                type == BLADERF_RF_EVT_TX_RFIC_STATE_VALID) {
                fprintf(stderr, "disabled TX retune unexpectedly reached "
                                "valid event %u\n", (unsigned)type);
                return BLADERF_ERR_UNEXPECTED;
            }
        }
        *cursor = next;
        const struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000};
        nanosleep(&pause, NULL);
    }
}

static int current_event_cursor(struct bladerf *dev,
                                struct bladerf_rf_event *events,
                                uint64_t *cursor)
{
    uint32_t count = 0;
    bool history_complete = false;
    uint64_t next = 0;
    int status = bladerf_rf_events_get_since(
        dev, 0, events, BLADERF_RF_EVENT_HISTORY_SIZE,
        &count, &next, &history_complete);
    if (status != 0 || !history_complete) {
        return status != 0 ? status : BLADERF_ERR_UNEXPECTED;
    }
    *cursor = next;
    return 0;
}

/* Discard old history after a long RX observer transition, then require fresh
 * TX certificate events from the tune that follows it. */
static int reset_event_cursor(struct bladerf *dev,
                              struct bladerf_rf_event *events,
                              uint64_t *cursor)
{
    uint32_t count = 0;
    uint64_t next = 0;
    bool history_complete = false;
    int status = bladerf_rf_events_get_since(
        dev, 0, events, BLADERF_RF_EVENT_HISTORY_SIZE,
        &count, &next, &history_complete);
    if (status == 0) {
        *cursor = next;
    }
    return status;
}

static void dump_tx_event_history(struct bladerf *dev)
{
    struct bladerf_rf_event events[BLADERF_RF_EVENT_HISTORY_SIZE];
    uint32_t count = 0;
    uint64_t next = 0;
    bool complete = false;
    const int status = bladerf_rf_events_get_since(
        dev, 0, events, BLADERF_RF_EVENT_HISTORY_SIZE,
        &count, &next, &complete);
    if (status != 0 || !complete) {
        fprintf(stderr, "TX event history unavailable: %s (%d), next=%" PRIu64
                        " complete=%u\n",
                bladerf_strerror(status), status, next, complete);
        return;
    }
    fprintf(stderr, "TX event history: count=%u next=%" PRIu64 "\n",
            count, next);
    for (uint32_t i = 0; i < count; ++i) {
        fprintf(stderr, "  event type=%u txn=%u epoch=%u error=%d "
                        "status=0x%08x flags=0x%x ts=%" PRIu64 "\n",
                (unsigned)events[i].event_type, events[i].transaction_id,
                events[i].epoch_id, events[i].error_code,
                events[i].rfic_status, events[i].flags,
                events[i].fpga_timestamp);
    }
}

int main(void)
{
    struct bladerf *dev = NULL;
    struct tx_feeder feeder = {0};
    pthread_t feeder_thread;
    struct bladerf_rf_event events[BLADERF_RF_EVENT_HISTORY_SIZE];
    struct rx_observer_reader rx_reader = {0};
    pthread_t rx_reader_thread;
    const char *mode_env = getenv("BLADERF_TEST_RX_TRANSITION_STALL");
    char mode_storage[96];
    const char *mode = mode_storage;
    uint64_t cursor = 0;
    uint32_t count = 0;
    bool history_complete = false;
    bool expected_event_seen = false;
    bool calibration_status_reported = false;
    bool link_fault_seen = false;
    bool rfics_invalid_seen = false;
    bool feeder_started = false;
    bool rx_reader_started = false;
    bladerf_frequency tx_recovery_frequency = 0;
    int status;
    const char *tx_rate_env = getenv("BLADERF_TX_TEST_SAMPLE_RATE");
    const char *tx_bandwidth_env = getenv("BLADERF_TX_TEST_BANDWIDTH");
    bladerf_sample_rate tx_sample_rate = 1000000;
    bladerf_bandwidth tx_bandwidth = 1500000;
    if (tx_rate_env != NULL) {
        tx_sample_rate = strtoull(tx_rate_env, NULL, 0);
    }
    if (tx_bandwidth_env != NULL) {
        tx_bandwidth = strtoul(tx_bandwidth_env, NULL, 0);
    }

    if (mode_env == NULL) {
        fprintf(stderr, "set BLADERF_TEST_RX_TRANSITION_STALL to a TX mode\n");
        return 2;
    }
    if (snprintf(mode_storage, sizeof(mode_storage), "%s", mode_env) >=
        (int)sizeof(mode_storage)) {
        fprintf(stderr, "TX runtime injection mode is too long\n");
        return 2;
    }
    if (strcmp(mode, "RUNTIME_TX_RFIC_STATUS_READ_FAILURE") != 0 &&
        strcmp(mode, "RUNTIME_TX_RFPLL_UNLOCKED") != 0 &&
        strcmp(mode, "RUNTIME_REFERENCE_PLL_UNLOCKED") != 0 &&
        strcmp(mode, "RUNTIME_TX_ENSM_NOT_TX") != 0 &&
        strcmp(mode, "RUNTIME_TX_BBPLL_UNLOCKED") != 0 &&
        strcmp(mode, "RUNTIME_TX_CALIBRATION_ACTIVE") != 0 &&
        strcmp(mode, "RUNTIME_TX_LINK_FAULT_NO_RFIC_READ") != 0 &&
        strcmp(mode, "RUNTIME_TX_RETUNE_FAILURE") != 0 &&
        strcmp(mode, "RUNTIME_TX_RETUNE_FAILURE_TX_DISABLED") != 0 &&
        strcmp(mode, "RUNTIME_TX_RFIC_REGISTER_WRITE") != 0 &&
        strcmp(mode, "RUNTIME_TX_GAIN_SETTER") != 0 &&
        strcmp(mode, "RUNTIME_TX_QUEUE_CLEAR_FAILURE") != 0 &&
        strcmp(mode, "RUNTIME_TX_GATE_STATUS_READ_FAILURE") != 0 &&
        strcmp(mode, "RUNTIME_TX_GATE_RELEASE_FAILURE") != 0 &&
        strcmp(mode, "RUNTIME_TX_GATE_ABORT_FAILURE") != 0 &&
        strcmp(mode, "RUNTIME_TX_TRANSPORT_FAILURE_DURING_RELEASE") != 0) {
        fprintf(stderr, "unsupported TX runtime injection mode: %s\n", mode);
        return 2;
    }

    status = bladerf_open(&dev, NULL);
    if (status != 0) {
        fprintf(stderr, "open device: %s (%d)\n",
                bladerf_strerror(status), status);
        goto cleanup;
    }
    feeder.dev = dev;
    feeder.metadata_format = mode_needs_tx_timestamp(mode);
    status = current_event_cursor(dev, events, &cursor);
    if (status != 0) goto cleanup;
    bladerf_log_set_verbosity(BLADERF_LOG_LEVEL_WARNING);
    status = bladerf_set_sample_rate(dev, BLADERF_CHANNEL_TX(0),
                                     tx_sample_rate, NULL);
    if (status != 0) {
        fprintf(stderr, "set TX sample rate: %s (%d)\n",
                bladerf_strerror(status), status);
        goto cleanup;
    }
    status = bladerf_set_bandwidth(dev, BLADERF_CHANNEL_TX(0),
                                   tx_bandwidth, NULL);
    if (status != 0) {
        fprintf(stderr, "set TX bandwidth: %s (%d)\n",
                bladerf_strerror(status), status);
        goto cleanup;
    }
    if (mode_needs_tx_timestamp(mode)) {
        status = prepare_rx_x2_observer(dev);
        if (status != 0) {
            fprintf(stderr, "prepare RX_X2 timestamp observer failed: %s\n",
                    bladerf_strerror(status));
            goto cleanup;
        }
    }
    status = bladerf_sync_config(dev, BLADERF_TX_X1,
                                 mode_needs_tx_timestamp(mode)
                                     ? BLADERF_FORMAT_SC16_Q11_META
                                     : BLADERF_FORMAT_SC16_Q11,
                                 16, 8192, 8, 0);
    if (status != 0) {
        fprintf(stderr, "configure TX sync stream: %s (%d)\n",
                bladerf_strerror(status), status);
        goto cleanup;
    }

    /* Establish a clean TX certificate before enabling a monitor injection.
     * Otherwise the injected state can invalidate the initial tune before
     * the test has ever admitted a valid TX buffer. */
    if (unsetenv("BLADERF_TEST_RX_TRANSITION_STALL") != 0) {
        status = BLADERF_ERR_UNEXPECTED;
        goto cleanup;
    }
    status = bladerf_enable_module(dev, BLADERF_CHANNEL_TX(0), true);
    if (status != 0) {
        fprintf(stderr, "enable TX module: %s (%d)\n",
                bladerf_strerror(status), status);
        goto cleanup;
    }
    /* TX startup calibration and ENSM entry are asynchronous observations.
     * Do not submit samples or request another retune until the monitor has
     * certified the newly enabled RFIC path. */
    /* Rate/bandwidth changes fence the previous TX RFIC certificate. Reapply
     * the current host-mode LO and wait for positive RFIC/retune evidence in
     * every fault scenario before enabling the requested injection. */
    bladerf_frequency current_frequency = 0;
    status = bladerf_get_frequency(dev, BLADERF_CHANNEL_TX(0),
                                   &current_frequency);
    if (status != 0) {
        fprintf(stderr, "TX frequency readback baseline failed: %s\n",
                bladerf_strerror(status));
    }
    if (status == 0) {
        tx_recovery_frequency = current_frequency;
        status = bladerf_set_frequency(dev, BLADERF_CHANNEL_TX(0),
                                       current_frequency);
        if (status != 0) {
            fprintf(stderr, "TX direct tune baseline failed: %s\n",
                    bladerf_strerror(status));
        }
    }
    if (status == 0) {
        status = wait_for_event(dev, events, &cursor,
                                BLADERF_RF_EVT_TX_RFIC_STATE_VALID);
    }
    if (status == 0) {
        status = wait_for_event(dev, events, &cursor,
                                BLADERF_RF_EVT_TX_RETUNE_VALID);
    }
    if (status == 0) {
        status = submit_tx_feeder_buffer(&feeder);
        if (status != 0) {
            fprintf(stderr, "valid TX baseline buffer failed: %s (%d)\n",
                    bladerf_strerror(status), status);
        }
    }
    if (status != 0) {
        fprintf(stderr, "TX RFIC did not become valid after enable: %s\n",
                bladerf_strerror(status));
        dump_tx_event_history(dev);
        goto cleanup;
    }
    if (!mode_needs_tx_timestamp(mode)) {
        status = start_tx_feeder(&feeder, &feeder_thread, &feeder_started);
        if (status != 0) {
            fprintf(stderr, "TX feeder did not accept baseline buffers: %s (%d)\n",
                    bladerf_strerror(status), status);
            dump_tx_event_history(dev);
            goto cleanup;
        }
    }
    if (setenv("BLADERF_TEST_RX_TRANSITION_STALL", mode, 1) != 0) {
        status = BLADERF_ERR_UNEXPECTED;
        goto cleanup;
    }
    if (strcmp(mode, "RUNTIME_TX_QUEUE_CLEAR_FAILURE") == 0 ||
        strcmp(mode, "RUNTIME_TX_RETUNE_FAILURE") == 0 ||
        strcmp(mode, "RUNTIME_TX_RETUNE_FAILURE_TX_DISABLED") == 0 ||
        strcmp(mode, "RUNTIME_TX_GATE_STATUS_READ_FAILURE") == 0 ||
        strcmp(mode, "RUNTIME_TX_GATE_RELEASE_FAILURE") == 0 ||
        strcmp(mode, "RUNTIME_TX_GATE_ABORT_FAILURE") == 0 ||
        strcmp(mode, "RUNTIME_TX_TRANSPORT_FAILURE_DURING_RELEASE") == 0) {
        status = bladerf_set_frequency(dev, BLADERF_CHANNEL_TX(0),
                                       915000000);
        if (status == 0) {
            status = wait_for_event(dev, events, &cursor,
                                    BLADERF_RF_EVT_TX_RFIC_STATE_VALID);
        }
        if (status == 0) {
            status = wait_for_event(dev, events, &cursor,
                                    BLADERF_RF_EVT_TX_RETUNE_VALID);
        }
        if (status == 0) {
            status = qualify_rx_x2_observer(dev, &rx_reader,
                                            &rx_reader_thread);
        }
        if (status == 0) {
            rx_reader_started = true;
            status = reset_event_cursor(dev, events, &cursor);
        }
        if (status != 0) {
            uint64_t next = cursor;
            uint32_t event_count = 0;
            bool complete = false;
            const int history_status = bladerf_rf_events_get_since(
                dev, cursor, events, BLADERF_RF_EVENT_HISTORY_SIZE,
                &event_count, &next, &complete);
            fprintf(stderr, "could not establish TX frequency baseline: %s\n",
                    bladerf_strerror(status));
            if (history_status == 0) {
                for (uint32_t i = 0; i < event_count; ++i) {
                    fprintf(stderr,
                            "  event type=%u epoch=%u requested=%" PRIu64
                            " readback=%" PRIu64 " error=%d flags=0x%x\n",
                            (unsigned)events[i].event_type, events[i].epoch_id,
                            events[i].requested_rx_lo_hz,
                            events[i].readback_rx_lo_hz,
                            events[i].error_code, events[i].flags);
                }
            }
            goto cleanup;
        }
    }
    if (strcmp(mode, "RUNTIME_TX_RFIC_REGISTER_WRITE") == 0 ||
        strcmp(mode, "RUNTIME_TX_GAIN_SETTER") == 0) {
        status = submit_tx_feeder_buffer(&feeder);
        if (status != 0) {
            fprintf(stderr, "TX stream was not valid before setter fault: %s\n",
                    bladerf_strerror(status));
            dump_tx_event_history(dev);
            goto cleanup;
        }
    }
    if (strcmp(mode, "RUNTIME_TX_RETUNE_FAILURE_TX_DISABLED") == 0) {
        bladerf_frequency frequency = 0;
        bladerf_timestamp timestamp = 0;
        struct bladerf_quick_tune quick_tune;
        bladerf_rf_event_type failure_event = BLADERF_RF_EVT_CONFIG_ACCEPTED;

        status = bladerf_get_frequency(dev, BLADERF_CHANNEL_TX(0),
                                       &frequency);
        if (status == 0) {
            status = bladerf_get_quick_tune(dev, BLADERF_CHANNEL_TX(0),
                                            &quick_tune);
        }
        if (status == 0) {
            status = bladerf_get_timestamp(dev, BLADERF_TX, &timestamp);
        }
        if (status == 0) {
            status = bladerf_schedule_retune(
                dev, BLADERF_CHANNEL_TX(0),
                timestamp + 100000,
                frequency, &quick_tune);
        }
        if (status == 0) {
            status = bladerf_enable_module(dev, BLADERF_CHANNEL_TX(0), false);
        }
        if (status == 0) {
            status = wait_for_tx_retune_terminal_event(
                dev, events, &cursor, &failure_event);
        }
        if (status != 0) {
            fprintf(stderr,
                    "disabled-TX retune failure was not reported: %s\n",
                    bladerf_strerror(status));
            goto cleanup;
        }
        printf("TX retune while disabled: PASS (fail-closed event type=%u)\n",
               (unsigned)failure_event);
        goto cleanup;
    }

    if (!feeder_started && !mode_needs_tx_timestamp(mode) &&
        strcmp(mode, "RUNTIME_TX_RETUNE_FAILURE_TX_DISABLED") != 0) {
        status = start_tx_feeder(&feeder, &feeder_thread, &feeder_started);
        if (status != 0) {
            fprintf(stderr, "TX feeder did not accept a buffer: %s (%d)\n",
                    bladerf_strerror(status), status);
            dump_tx_event_history(dev);
            goto cleanup;
        }
    }

    if (strcmp(mode, "RUNTIME_TX_RETUNE_FAILURE") == 0) {
        bladerf_frequency frequency = 0;
        bladerf_timestamp timestamp = 0;
        bladerf_timestamp target_timestamp = 0;
        struct bladerf_quick_tune quick_tune;

        status = bladerf_get_frequency(dev, BLADERF_CHANNEL_TX(0),
                                       &frequency);
        if (status == 0) {
            status = bladerf_get_quick_tune(dev, BLADERF_CHANNEL_TX(0),
                                            &quick_tune);
        }
        if (status == 0) {
            status = bladerf_get_timestamp(dev, BLADERF_TX, &timestamp);
        }
        if (status == 0) {
            target_timestamp = timestamp + 100000;
            status = submit_tx_scheduled_hold(&feeder, target_timestamp);
        }
        if (status == 0) {
            status = bladerf_schedule_retune(dev, BLADERF_CHANNEL_TX(0),
                                             target_timestamp, frequency,
                                             &quick_tune);
        }
        if (status != 0) {
            fprintf(stderr, "could not schedule enabled TX retune fault: %s\n",
                    bladerf_strerror(status));
            goto cleanup;
        }
    }

    if (strcmp(mode, "RUNTIME_TX_QUEUE_CLEAR_FAILURE") == 0) {
        bladerf_frequency frequency = 0;
        bladerf_timestamp timestamp = 0;
        bladerf_timestamp target_timestamp = 0;
        struct bladerf_quick_tune quick_tune;

        fprintf(stderr, "queue-clear: read current tune\n");
        status = bladerf_get_frequency(dev, BLADERF_CHANNEL_TX(0),
                                       &frequency);
        if (status == 0) {
            status = bladerf_get_quick_tune(dev, BLADERF_CHANNEL_TX(0),
                                            &quick_tune);
        }
        if (status == 0) {
            status = bladerf_get_timestamp(dev, BLADERF_TX, &timestamp);
        }
        if (status == 0) {
            fprintf(stderr, "queue-clear: request baseline schedule\n");
            target_timestamp = timestamp + 100000;
            status = submit_tx_scheduled_hold(&feeder, target_timestamp);
        }
        if (status == 0) {
            status = bladerf_schedule_retune(
                dev, BLADERF_CHANNEL_TX(0), target_timestamp,
                frequency, &quick_tune);
        }
        if (status == 0) {
            fprintf(stderr, "queue-clear: wait baseline valid event\n");
            status = wait_for_event(dev, events, &cursor,
                                    BLADERF_RF_EVT_TX_RETUNE_VALID);
        }
        if (status != 0) {
            fprintf(stderr, "could not queue TX retune baseline: %s\n",
                    bladerf_strerror(status));
            goto cleanup;
        }

        /* Keep the scheduled item pending while direct tuning attempts its
         * queue clear. The test-build hook fails that cancellation. */
        fprintf(stderr, "queue-clear: inject cancellation failure\n");
        status = bladerf_set_frequency(dev, BLADERF_CHANNEL_TX(0), frequency);
        if (status != BLADERF_ERR_IO) {
            fprintf(stderr, "queue-clear fault returned %s (%d), expected IO\n",
                    bladerf_strerror(status), status);
            status = BLADERF_ERR_UNEXPECTED;
            goto cleanup;
        }
        fprintf(stderr, "queue-clear: wait completion-lost event\n");
        status = wait_for_event(dev, events, &cursor,
                                BLADERF_RF_EVT_TX_RETUNE_COMPLETION_LOST);
        if (status != 0) {
            fprintf(stderr, "queue-clear failure event missing: %s\n",
                    bladerf_strerror(status));
            goto cleanup;
        }
        fprintf(stderr, "queue-clear: verify TX blocked\n");
        status = submit_tx_feeder_buffer(&feeder);
        if (status != BLADERF_ERR_FPGA_OP) {
            fprintf(stderr, "TX samples were not rejected after queue-clear fault: %s\n",
                    bladerf_strerror(status));
            status = BLADERF_ERR_UNEXPECTED;
            goto cleanup;
        }

        if (setenv("BLADERF_TEST_RX_TRANSITION_STALL", "", 1) != 0) {
            status = BLADERF_ERR_UNEXPECTED;
            goto cleanup;
        }
        /* The one timestamped hold burst has ended by this point, so its TX
         * link epoch is no longer active. Reopen and certify a fresh stream
         * epoch before claiming that recovery restored sample admission. */
        fprintf(stderr, "queue-clear: reopen TX stream for recovery\n");
        if (rx_reader_started) {
            atomic_store(&rx_reader.stop, true);
            (void)pthread_join(rx_reader_thread, NULL);
            rx_reader_started = false;
        }
        bladerf_close(dev);
        dev = NULL;
        status = bladerf_open(&dev, NULL);
        if (status == 0) {
            status = current_event_cursor(dev, events, &cursor);
        }
        if (status == 0) {
            status = bladerf_set_sample_rate(
                dev, BLADERF_CHANNEL_TX(0), tx_sample_rate, NULL);
        }
        if (status == 0) {
            status = bladerf_set_bandwidth(
                dev, BLADERF_CHANNEL_TX(0), tx_bandwidth, NULL);
        }
        if (status == 0) {
            status = bladerf_sync_config(
                dev, BLADERF_TX_X1, BLADERF_FORMAT_SC16_Q11_META,
                16, 8192, 8, 0);
        }
        if (status == 0) {
            status = bladerf_enable_module(
                dev, BLADERF_CHANNEL_TX(0), true);
        }
        if (status == 0) {
            status = bladerf_set_frequency(
                dev, BLADERF_CHANNEL_TX(0), frequency);
        }
        if (status == 0) {
            status = wait_for_event(dev, events, &cursor,
                                    BLADERF_RF_EVT_TX_RFIC_STATE_VALID);
        }
        if (status == 0) {
            fprintf(stderr, "queue-clear: wait recovery valid event\n");
            status = wait_for_event(dev, events, &cursor,
                                    BLADERF_RF_EVT_TX_RETUNE_VALID);
        }
        if (status == 0) {
            fprintf(stderr, "queue-clear: verify TX resumed\n");
            status = submit_tx_feeder_buffer(&feeder);
        }
        if (status != 0) {
            fprintf(stderr, "TX did not recover after queue-clear fault: %s\n",
                    bladerf_strerror(status));
            goto cleanup;
        }

        printf("TX queue-clear containment: PASS (fault rejected, retuned, resumed)\n");
        status = 0;
        goto cleanup;
    }

    if (strcmp(mode, "RUNTIME_TX_GATE_STATUS_READ_FAILURE") == 0 ||
        strcmp(mode, "RUNTIME_TX_GATE_RELEASE_FAILURE") == 0 ||
        strcmp(mode, "RUNTIME_TX_GATE_ABORT_FAILURE") == 0 ||
        strcmp(mode, "RUNTIME_TX_TRANSPORT_FAILURE_DURING_RELEASE") == 0) {
        bladerf_frequency frequency = 0;
        bladerf_timestamp timestamp = 0;
        bladerf_timestamp target_timestamp = 0;
        uint64_t scheduled_cursor = cursor;
        struct bladerf_quick_tune quick_tune;

        status = bladerf_get_frequency(dev, BLADERF_CHANNEL_TX(0),
                                       &frequency);
        if (status == 0) {
            status = bladerf_get_quick_tune(dev, BLADERF_CHANNEL_TX(0),
                                            &quick_tune);
        }
        if (status == 0) {
            status = bladerf_get_timestamp(dev, BLADERF_TX, &timestamp);
        }
        if (status == 0) {
            const bool transport_fault = strcmp(
                mode, "RUNTIME_TX_TRANSPORT_FAILURE_DURING_RELEASE") == 0;
            if (setenv(transport_fault
                           ? "BLADERF_TEST_TX_TRANSPORT_FAILURE_ON_RELEASE"
                           : "BLADERF_TEST_TX_GATE_FAULT",
                       transport_fault ? "1" : mode, 1) != 0) {
                status = BLADERF_ERR_UNEXPECTED;
            }
        }
        if (status == 0) {
            target_timestamp = timestamp + 100000;
            status = submit_tx_scheduled_hold(&feeder, target_timestamp);
        }
        if (status == 0) {
            status = bladerf_schedule_retune(
                dev, BLADERF_CHANNEL_TX(0), target_timestamp,
                frequency, &quick_tune);
        }
        if (status == 0) {
            const bladerf_rf_event_type expected_event = strcmp(
                mode, "RUNTIME_TX_TRANSPORT_FAILURE_DURING_RELEASE") == 0
                    ? BLADERF_RF_EVT_TX_TRANSPORT_ERROR
                    : (strcmp(mode, "RUNTIME_TX_GATE_ABORT_FAILURE") == 0
                           ? BLADERF_RF_EVT_TX_STATUS_UNAVAILABLE
                           : BLADERF_RF_EVT_TX_RETUNE_COMPLETION_LOST);
            status = wait_for_event(dev, events, &cursor, expected_event);
        }
        if (status != 0) {
            fprintf(stderr, "TX gate fault was not contained: %s\n",
                    bladerf_strerror(status));
            goto cleanup;
        }
        if (strcmp(mode,
                   "RUNTIME_TX_TRANSPORT_FAILURE_DURING_RELEASE") == 0) {
            bool history_complete = false;
            bool tx_valid_seen = false;
            uint64_t next = scheduled_cursor;
            status = bladerf_rf_events_get_since(
                dev, scheduled_cursor, events, BLADERF_RF_EVENT_HISTORY_SIZE,
                &count, &next, &history_complete);
            if (status != 0 || !history_complete) {
                status = status != 0 ? status : BLADERF_ERR_UNEXPECTED;
                goto cleanup;
            }
            for (uint32_t i = 0; i < count; ++i) {
                tx_valid_seen |= events[i].event_type ==
                                 BLADERF_RF_EVT_TX_RETUNE_VALID;
            }
            cursor = next;
            if (tx_valid_seen) {
                fprintf(stderr,
                        "TX-valid preceded transport failure containment\n");
                status = BLADERF_ERR_UNEXPECTED;
                goto cleanup;
            }
            if (feeder_started) {
                while (atomic_load(&feeder.terminal_status) == 0) {
                    const struct timespec pause = {.tv_sec = 0,
                                                   .tv_nsec = 1000000};
                    nanosleep(&pause, NULL);
                }
                status = atomic_load(&feeder.terminal_status);
            } else {
                status = submit_tx_feeder_buffer(&feeder);
            }
            if (status != BLADERF_ERR_FPGA_OP) {
                fprintf(stderr,
                        "TX submission remained admitted after transport fault\n");
                status = BLADERF_ERR_UNEXPECTED;
                goto cleanup;
            }
            printf("TX transport fault during mute release: PASS (no false TX-valid)\n");
            status = 0;
            goto cleanup;
        }
        if (strcmp(mode, "RUNTIME_TX_GATE_ABORT_FAILURE") == 0) {
            bool completion_lost_seen = false;
            uint64_t next = scheduled_cursor;
            status = bladerf_rf_events_get_since(
                dev, scheduled_cursor, events, BLADERF_RF_EVENT_HISTORY_SIZE,
                &count, &next, &history_complete);
            if (status == 0 && history_complete) {
                for (uint32_t i = 0; i < count; ++i) {
                    completion_lost_seen |= events[i].event_type ==
                        BLADERF_RF_EVT_TX_RETUNE_COMPLETION_LOST;
                }
                cursor = next;
            } else if (status == 0) {
                status = BLADERF_ERR_UNEXPECTED;
            }
            if (status == 0 && !completion_lost_seen) {
                status = BLADERF_ERR_UNEXPECTED;
            }
            if (status != 0) {
                fprintf(stderr,
                        "TX gate abort failure lacked completion-loss/status evidence\n");
                goto cleanup;
            }
            status = bladerf_set_frequency(dev, BLADERF_CHANNEL_TX(0),
                                           frequency);
            if (status != BLADERF_ERR_WOULD_BLOCK) {
                fprintf(stderr, "TX retune was not blocked without mute ACK: %s\n",
                        bladerf_strerror(status));
                status = BLADERF_ERR_UNEXPECTED;
                goto cleanup;
            }
            status = bladerf_set_bandwidth(dev, BLADERF_CHANNEL_TX(0),
                                           1500000, NULL);
            if (status != BLADERF_ERR_WOULD_BLOCK) {
                fprintf(stderr, "TX RF configuration bypassed missing mute ACK: %s\n",
                        bladerf_strerror(status));
                status = BLADERF_ERR_UNEXPECTED;
                goto cleanup;
            }
            status = bladerf_set_gain(dev, BLADERF_CHANNEL_TX(0), -20);
            if (status != BLADERF_ERR_WOULD_BLOCK) {
                fprintf(stderr, "TX gain setter bypassed missing mute ACK: %s\n",
                        bladerf_strerror(status));
                status = BLADERF_ERR_UNEXPECTED;
                goto cleanup;
            }
            status = submit_tx_feeder_buffer(&feeder);
            if (status != BLADERF_ERR_FPGA_OP) {
                fprintf(stderr, "TX samples were not rejected without mute ACK\n");
                status = BLADERF_ERR_UNEXPECTED;
                goto cleanup;
            }
            printf("TX gate abort failure: PASS (status unavailable, retune blocked)\n");
            status = 0;
            goto cleanup;
        }
        status = submit_tx_feeder_buffer(&feeder);
        if (status != BLADERF_ERR_FPGA_OP) {
            fprintf(stderr, "TX samples were not rejected after gate fault: %s\n",
                    bladerf_strerror(status));
            status = BLADERF_ERR_UNEXPECTED;
            goto cleanup;
        }
        if (setenv("BLADERF_TEST_TX_GATE_FAULT", "", 1) != 0) {
            status = BLADERF_ERR_UNEXPECTED;
            goto cleanup;
        }
        status = bladerf_set_frequency(dev, BLADERF_CHANNEL_TX(0), frequency);
        if (status == 0) {
            status = wait_for_event(dev, events, &cursor,
                                    BLADERF_RF_EVT_TX_RETUNE_VALID);
        }
        if (status == 0) {
            status = submit_tx_feeder_buffer(&feeder);
        }
        if (status != 0) {
            fprintf(stderr, "TX did not recover after gate fault: %s\n",
                    bladerf_strerror(status));
            goto cleanup;
        }
        printf("TX gate containment: PASS mode=%s (abort, retune, resume)\n",
               mode);
        status = 0;
        goto cleanup;
    }

    if (strcmp(mode, "RUNTIME_TX_GAIN_SETTER") == 0) {
        status = bladerf_set_gain(dev, BLADERF_CHANNEL_TX(0), -20);
        if (status != 0) {
            fprintf(stderr, "TX gain setter failed: %s\n",
                    bladerf_strerror(status));
            goto cleanup;
        }
        /* The TX configuration fence publishes this event synchronously
         * inside bladerf_set_gain(). If it is absent now, polling forever
         * cannot make this mode pass. */
        status = bladerf_rf_events_get_since(
            dev, cursor, events, BLADERF_RF_EVENT_HISTORY_SIZE,
            &count, &cursor, &history_complete);
        if (status != 0 || !history_complete) {
            fprintf(stderr, "TX gain setter event history failed: %s (%d)\n",
                    bladerf_strerror(status), status);
            goto cleanup;
        }
        expected_event_seen = false;
        for (uint32_t i = 0; i < count; ++i) {
            if (events[i].event_type ==
                BLADERF_RF_EVT_TX_CONFIGURATION_INVALID) {
                expected_event_seen = true;
            }
        }
        if (!expected_event_seen) {
            fprintf(stderr, "TX gain setter returned without publishing "
                            "TX_CONFIGURATION_INVALID\n");
            dump_tx_event_history(dev);
            status = BLADERF_ERR_UNEXPECTED;
            goto cleanup;
        }
    }

    if (strcmp(mode, "RUNTIME_TX_RFIC_REGISTER_WRITE") == 0) {
        uint8_t tx_clock_data_delay;
        /* Rewrite the existing value while TX is live. The RFIC value stays
         * unchanged; the public raw-register path must still revoke TX
         * admission and acknowledge the FPGA mute before the SPI write. */
        status = bladerf_get_frequency(dev, BLADERF_CHANNEL_TX(0),
                                       &tx_recovery_frequency);
        if (status != 0) goto cleanup;
        status = bladerf_get_rfic_register(dev, 0x007,
                                           &tx_clock_data_delay);
        if (status != 0) {
            fprintf(stderr, "TX RFIC register read failed: %s\n",
                    bladerf_strerror(status));
        }
        if (status == 0) {
            status = bladerf_set_rfic_register(dev, 0x007,
                                               tx_clock_data_delay);
            if (status != 0) {
                fprintf(stderr, "TX RFIC register write failed: %s\n",
                        bladerf_strerror(status));
            }
        }
        if (status != 0) goto cleanup;
    }

    while (!expected_event_seen) {
        uint64_t next = cursor;
        status = bladerf_rf_events_get_since(
            dev, cursor, events, BLADERF_RF_EVENT_HISTORY_SIZE,
            &count, &next, &history_complete);
        if (status != 0 || !history_complete) {
            fprintf(stderr, "RF event history incomplete: %s\n",
                    bladerf_strerror(status));
            status = status != 0 ? status : BLADERF_ERR_UNEXPECTED;
            goto cleanup;
        }
        cursor = next;
        for (uint32_t i = 0; i < count; ++i) {
            const struct bladerf_rf_event *event = &events[i];
            if (event->event_type == BLADERF_RF_EVT_TX_LINK_FAULT) {
                link_fault_seen = true;
            } else if (event->event_type ==
                       BLADERF_RF_EVT_TX_RFIC_STATE_INVALID) {
                rfics_invalid_seen = true;
                if (strcmp(mode, "RUNTIME_TX_CALIBRATION_ACTIVE") == 0 &&
                    (event->rfic_status >> 24) != 0) {
                    calibration_status_reported = true;
                }
            }

            if ((strcmp(mode, "RUNTIME_TX_RFIC_STATUS_READ_FAILURE") == 0 ||
                 strcmp(mode, "RUNTIME_TX_LINK_FAULT_NO_RFIC_READ") == 0) &&
                event->event_type == BLADERF_RF_EVT_TX_STATUS_UNAVAILABLE) {
                expected_event_seen = true;
            } else if ((strcmp(mode, "RUNTIME_TX_RFPLL_UNLOCKED") == 0 ||
                        strcmp(mode, "RUNTIME_TX_ENSM_NOT_TX") == 0 ||
                        strcmp(mode, "RUNTIME_TX_BBPLL_UNLOCKED") == 0 ||
                        strcmp(mode, "RUNTIME_TX_CALIBRATION_ACTIVE") == 0) &&
                       event->event_type ==
                           BLADERF_RF_EVT_TX_RFIC_STATE_INVALID) {
                expected_event_seen = true;
            } else if (strcmp(mode, "RUNTIME_TX_RETUNE_FAILURE") == 0 &&
                       event->event_type ==
                           BLADERF_RF_EVT_TX_RETUNE_FAILED) {
                expected_event_seen = true;
            } else if ((strcmp(mode, "RUNTIME_TX_RFIC_REGISTER_WRITE") == 0 ||
                        strcmp(mode, "RUNTIME_TX_GAIN_SETTER") == 0) &&
                       event->event_type ==
                           BLADERF_RF_EVT_TX_CONFIGURATION_INVALID) {
                expected_event_seen = true;
            } else if (strcmp(mode,
                              "RUNTIME_REFERENCE_PLL_UNLOCKED") == 0 &&
                       event->event_type ==
                           BLADERF_RF_EVT_TX_CONFIGURATION_INVALID) {
                expected_event_seen = true;
            }
        }
        if (!expected_event_seen) {
            const int feeder_status = feeder_started
                ? atomic_load(&feeder.terminal_status) : 0;
            if (feeder_status != 0) {
                fprintf(stderr, "TX feeder terminated before expected event "
                                "for %s: %s (%d)\n",
                        mode, bladerf_strerror(feeder_status), feeder_status);
                dump_tx_event_history(dev);
                status = feeder_status;
                goto cleanup;
            }
            const struct timespec pause = {.tv_sec = 0,
                                           .tv_nsec = 1000000};
            nanosleep(&pause, NULL);
        }
    }

    if (!expected_event_seen) {
        fprintf(stderr, "expected TX fault event not observed for %s\n", mode);
        status = BLADERF_ERR_UNEXPECTED;
        goto cleanup;
    }
    if (strcmp(mode, "RUNTIME_TX_CALIBRATION_ACTIVE") == 0 &&
        !calibration_status_reported) {
        fprintf(stderr, "TX calibration status was omitted from RFIC event\n");
        status = BLADERF_ERR_UNEXPECTED;
        goto cleanup;
    }
    if (strcmp(mode, "RUNTIME_TX_LINK_FAULT_NO_RFIC_READ") == 0 &&
        (!link_fault_seen || rfics_invalid_seen)) {
        fprintf(stderr, "skipped RFIC reads were misclassified: link_fault=%u "
                "rfic_invalid=%u\n", link_fault_seen, rfics_invalid_seen);
        status = BLADERF_ERR_UNEXPECTED;
        goto cleanup;
    }

    if (strcmp(mode, "RUNTIME_TX_RETUNE_FAILURE") == 0) {
        status = submit_tx_feeder_buffer(&feeder);
        if (status != BLADERF_ERR_FPGA_OP) {
            fprintf(stderr,
                    "TX samples were not rejected after retune fault: %s (%d)\n",
                    bladerf_strerror(status), status);
            status = BLADERF_ERR_UNEXPECTED;
            goto cleanup;
        }
        printf("TX scheduled retune failure: PASS (fault event, TX blocked)\n");
        status = 0;
        goto cleanup;
    }

    /* The event is the authoritative fault completion signal. Stop the
     * background producer, then synchronously prove that a new TX request is
     * rejected. Waiting for the producer to observe a terminal status can
     * spin forever when sync TX reports WOULD_BLOCK for a faulted stream. */
    if (feeder_started) {
        atomic_store(&feeder.stop, true);
        (void)pthread_join(feeder_thread, NULL);
        feeder_started = false;
    }
    status = submit_tx_feeder_buffer(&feeder);
    if (status != BLADERF_ERR_FPGA_OP) {
        fprintf(stderr, "TX feeder was not rejected after fault: %s (%d)\n",
                status != 0 ? bladerf_strerror(status) : "still running",
                status);
        status = BLADERF_ERR_UNEXPECTED;
        goto cleanup;
    }

    if (strcmp(mode, "RUNTIME_TX_RFIC_REGISTER_WRITE") == 0 ||
        strcmp(mode, "RUNTIME_TX_GAIN_SETTER") == 0) {
        bladerf_frequency current_frequency = 0;

        /* A terminal fault may leave in-flight USB callbacks associated with
         * the old transport generation. Retire that stream and device handle
         * before checking whether a fresh TX session can submit IQ again. */
        /* A terminal stream fault requires retiring the sync stream and
         * reopening the handle before checking that TX can be certified
         * again. Use the LO recorded before the fault; a raw-register write
         * also invalidates the live frequency model. */
        current_frequency = tx_recovery_frequency;

        unsetenv("BLADERF_TEST_RX_TRANSITION_STALL");
        if (feeder_started) {
            atomic_store(&feeder.stop, true);
            (void)pthread_join(feeder_thread, NULL);
            feeder_started = false;
        }
        bladerf_close(dev);
        dev = NULL;

        status = bladerf_open(&dev, NULL);
        if (status == 0) {
            status = current_event_cursor(dev, events, &cursor);
        }
        if (status == 0) {
            status = bladerf_set_sample_rate(dev, BLADERF_CHANNEL_TX(0),
                                             tx_sample_rate, NULL);
        }
        if (status == 0) {
            status = bladerf_set_bandwidth(dev, BLADERF_CHANNEL_TX(0),
                                           tx_bandwidth, NULL);
        }
        if (status == 0) {
            status = bladerf_sync_config(dev, BLADERF_TX_X1,
                                         BLADERF_FORMAT_SC16_Q11,
                                         16, 8192, 8, 0);
        }
        if (status == 0) {
            status = bladerf_enable_module(dev, BLADERF_CHANNEL_TX(0), true);
        }
        if (status == 0) {
            status = bladerf_set_frequency(dev, BLADERF_CHANNEL_TX(0),
                                           current_frequency);
        }
        if (status == 0) {
            status = wait_for_event(dev, events, &cursor,
                                    BLADERF_RF_EVT_TX_RFIC_STATE_VALID);
        }
        if (status == 0) {
            status = wait_for_event(dev, events, &cursor,
                                    BLADERF_RF_EVT_TX_RETUNE_VALID);
        }
        if (status == 0) {
            status = submit_tx_feeder_buffer(&feeder);
        }
        if (status != 0) {
            fprintf(stderr, "TX did not recover after handle reopen: %s\n",
                    bladerf_strerror(status));
            goto cleanup;
        }
    }

    printf("TX RFIC fault path: PASS mode=%s link_fault=%u "
           "rfic_invalid=%u sync_tx=REJECTED\n",
           mode, link_fault_seen, rfics_invalid_seen);
    status = 0;

cleanup:
    unsetenv("BLADERF_TEST_RX_TRANSITION_STALL");
    unsetenv("BLADERF_TEST_TX_GATE_FAULT");
    if (feeder_started) {
        atomic_store(&feeder.stop, true);
        (void)pthread_join(feeder_thread, NULL);
    }
    if (rx_reader_started) {
        atomic_store(&rx_reader.stop, true);
        (void)pthread_join(rx_reader_thread, NULL);
        const int rx_status = atomic_load(&rx_reader.terminal_status);
        if (status == 0 && rx_status != 0) {
            status = rx_status;
        }
    }
    if (dev != NULL) {
        (void)bladerf_enable_module(dev, BLADERF_CHANNEL_TX(0), false);
        bladerf_close(dev);
    }
    if (status != 0) {
        fprintf(stderr, "TX RFIC fault test failed: %s (%d)\n",
                bladerf_strerror(status), status);
    }
    return status == 0 ? 0 : 1;
}
