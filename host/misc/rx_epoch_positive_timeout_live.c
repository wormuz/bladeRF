#define _POSIX_C_SOURCE 200809L
#include <libbladeRF.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SAMPLE_COUNT 4096
#define WAIT_TIMEOUT_MS 100

struct rx_test_config {
    bladerf_channel_layout layout;
    bladerf_channel transition_channel;
    bool paired;
    bool enable_rx1;
    bool enable_rx2;
    unsigned int sync_samples;
    const char *name;
};

static uint64_t monotonic_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int transition(struct bladerf *dev,
                      const struct rx_test_config *config, bool inject_stall,
                      const char *stage, struct bladerf_rf_event *event)
{
    struct bladerf_rx_transition_request request = {0};
    uint32_t transaction_id = 0;
    int status;

    request.target_frequency_hz = 1835000000ULL;
    request.required_events_mask = BLADERF_RF_REQUIRE_EPOCH_VALID;
    request.require_rx_data_valid = true;
    request.timeout_ms = 2000;
    status = bladerf_rx_transition_begin(dev, config->transition_channel,
                                         &request, &transaction_id);
    if (status != 0) {
        return status;
    }

    if (inject_stall) {
        const bool late_observation = strncmp(stage, "LATE_", 5) == 0;
        const char *variable = late_observation
            ? "BLADERF_TEST_RX_TRANSITION_LATE_OBSERVATION"
            : "BLADERF_TEST_RX_TRANSITION_STALL";
        if (setenv(variable, stage, 1) != 0) {
            return BLADERF_ERR_UNEXPECTED;
        }
    }
    const uint64_t started_ns = monotonic_ns();
    status = bladerf_rx_transition_wait(dev, transaction_id, event,
                                        inject_stall ? WAIT_TIMEOUT_MS : 2000);
    const uint64_t elapsed_ns = monotonic_ns() - started_ns;
    if (inject_stall) {
        unsetenv("BLADERF_TEST_RX_TRANSITION_STALL");
        unsetenv("BLADERF_TEST_RX_TRANSITION_LATE_OBSERVATION");
        if (status != BLADERF_ERR_TIMEOUT ||
            event->event_type != BLADERF_RF_EVT_ERROR ||
            event->error_code != BLADERF_ERR_TIMEOUT ||
            elapsed_ns < 90000000ULL || elapsed_ns > 1000000000ULL) {
            fprintf(stderr, "%s timeout contract mismatch: status=%s event=%d "
                    "error=%d elapsed_ms=%.3f\n", stage,
                    bladerf_strerror(status), event->event_type,
                    event->error_code, elapsed_ns / 1e6);
            return BLADERF_ERR_UNEXPECTED;
        }
        printf("%s positive timeout: PASS txn=%u elapsed_ms=%.3f\n",
               stage, transaction_id, elapsed_ns / 1e6);
        status = 0;
    } else if (status == 0 && event->fpga_state != BLADERF_RF_STATE_RX_DATA_VALID) {
        return BLADERF_ERR_UNEXPECTED;
    }
    return status;
}

static int assert_sync_withheld(struct bladerf *dev,
                                const struct rx_test_config *config)
{
    int16_t samples[SAMPLE_COUNT * 4];
    int16_t sentinel[SAMPLE_COUNT * 4];
    struct bladerf_metadata metadata = {0};
    memset(samples, 0x5a, sizeof(samples));
    memcpy(sentinel, samples, sizeof(sentinel));
    metadata.flags = BLADERF_META_FLAG_RX_NOW;

    const int status = bladerf_sync_rx(dev, samples, config->sync_samples,
                                       &metadata, 200);
    if (status != BLADERF_ERR_WOULD_BLOCK || metadata.actual_count != 0 ||
        memcmp(samples, sentinel, sizeof(samples)) != 0) {
        fprintf(stderr, "IQ escaped timed-out transition: status=%s count=%u "
                "buffer_unchanged=%d\n", bladerf_strerror(status),
                metadata.actual_count,
                memcmp(samples, sentinel, sizeof(samples)) == 0);
        return BLADERF_ERR_UNEXPECTED;
    }
    return 0;
}

static int assert_sync_valid(struct bladerf *dev,
                             const struct rx_test_config *config,
                             const struct bladerf_rf_event *event)
{
    int16_t samples[SAMPLE_COUNT * 4];
    uint64_t expected_timestamp = event->fpga_timestamp;
    unsigned int total = 0;
    for (unsigned int attempt = 0;
         total < config->sync_samples && attempt < 30;
         ++attempt) {
        struct bladerf_metadata metadata = {0};
        metadata.flags = BLADERF_META_FLAG_RX_NOW;
        const int status = bladerf_sync_rx(
            dev, samples + 2 * total, config->sync_samples - total,
            &metadata, 200);
        if (status == BLADERF_ERR_WOULD_BLOCK) {
            continue;
        }
        if (status == 0 &&
            (metadata.status & BLADERF_META_STATUS_OVERRUN) != 0) {
            /* An overrun read is explicitly invalid as a contiguous clip. */
            total = 0;
            expected_timestamp = event->fpga_timestamp;
            continue;
        }
        if (status != 0 || metadata.actual_count == 0 ||
            !metadata.rx_epoch_id_valid ||
            metadata.rx_epoch_id != event->epoch_id ||
            metadata.timestamp < event->fpga_timestamp ||
            (total != 0 && metadata.timestamp != expected_timestamp)) {
            fprintf(stderr, "recovery META mismatch: status=%s count=%u "
                    "status_flags=0x%x valid=%d epoch=%u expected=%u "
                    "timestamp=%llu expected_timestamp=%llu boundary=%llu\n",
                    bladerf_strerror(status), metadata.actual_count,
                    metadata.status, metadata.rx_epoch_id_valid,
                    metadata.rx_epoch_id, event->epoch_id,
                    (unsigned long long)metadata.timestamp,
                    (unsigned long long)expected_timestamp,
                    (unsigned long long)event->fpga_timestamp);
            return BLADERF_ERR_UNEXPECTED;
        }
        total += metadata.actual_count;
        expected_timestamp = metadata.timestamp +
            (config->paired ? metadata.actual_count / 2 :
                              metadata.actual_count);
    }
    if (total != config->sync_samples) {
        fprintf(stderr, "recovery block incomplete: received=%u expected=%u\n",
                total, config->sync_samples);
        return BLADERF_ERR_UNEXPECTED;
    }
    return 0;
}

/* A public low-level setter must never infer that a reservation belongs to
 * it merely because another thread currently holds one. Use an idempotent
 * bias-tee write so an incorrectly admitted call does not change board state. */
static int assert_legacy_setter_blocked_during_transition(
    struct bladerf *dev, const struct rx_test_config *config)
{
    struct bladerf_rx_transition_request request = {0};
    struct bladerf_rf_event event = {0};
    uint32_t transaction_id = 0;
    bool bias_enabled = false;
    int status = bladerf_get_bias_tee(
        dev, BLADERF_CHANNEL_RX(0), &bias_enabled);
    if (status != 0) {
        return status;
    }

    request.target_frequency_hz = 1835000000ULL;
    request.required_events_mask = BLADERF_RF_REQUIRE_EPOCH_VALID;
    request.require_rx_data_valid = true;
    request.timeout_ms = 2000;
    status = bladerf_rx_transition_begin(dev, config->transition_channel,
                                         &request, &transaction_id);
    if (status != 0) {
        return status;
    }

    status = bladerf_set_bias_tee(dev, BLADERF_CHANNEL_RX(0), bias_enabled);
    if (status != BLADERF_ERR_WOULD_BLOCK) {
        fprintf(stderr, "concurrent RX setter was not rejected: %s\n",
                bladerf_strerror(status));
        return BLADERF_ERR_UNEXPECTED;
    }

    status = bladerf_rx_transition_wait(dev, transaction_id, &event, 2000);
    if (status != 0 || event.fpga_state != BLADERF_RF_STATE_RX_DATA_VALID) {
        fprintf(stderr, "transition after rejected setter failed: %s state=%u\n",
                bladerf_strerror(status), event.fpga_state);
        return status != 0 ? status : BLADERF_ERR_UNEXPECTED;
    }
    status = assert_sync_valid(dev, config, &event);
    if (status == 0) {
        printf("concurrent low-level setter reservation: PASS (%s)\n",
               config->name);
    }
    return status;
}

int main(int argc, char **argv)
{
    static const char *const stages[] = {
        "PLL", "ENSM", "BBPLL", "EPOCH",
        "LATE_PLL", "LATE_ENSM", "LATE_BBPLL", "LATE_COMPLETE",
        "LATE_EPOCH", "LATE_TIMESTAMP", "LATE_LINK_STATUS",
        "LATE_HOST_FENCE",
    };
    struct rx_test_config config = {
        .layout = BLADERF_RX_X1,
        .transition_channel = BLADERF_CHANNEL_RX(1),
        .paired = false,
        .enable_rx1 = false,
        .enable_rx2 = true,
        .sync_samples = SAMPLE_COUNT,
        .name = "RX2",
    };
    struct bladerf *dev = NULL;
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "RX1") != 0 &&
                     strcmp(argv[1], "RX2") != 0 &&
                     strcmp(argv[1], "BOTH") != 0)) {
        fprintf(stderr, "usage: %s [RX1|RX2|BOTH]\n", argv[0]);
        return 2;
    }
    if (argc == 2 && strcmp(argv[1], "RX1") == 0) {
        config.transition_channel = BLADERF_CHANNEL_RX(0);
        config.enable_rx1 = true;
        config.enable_rx2 = false;
        config.name = "RX1";
    } else if (argc == 2 && strcmp(argv[1], "BOTH") == 0) {
        config.layout = BLADERF_RX_X2;
        config.transition_channel = BLADERF_CHANNEL_RX(0);
        config.paired = true;
        config.enable_rx1 = true;
        config.enable_rx2 = true;
        config.sync_samples = SAMPLE_COUNT * 2;
        config.name = "RX1+RX2";
    }
    int status = bladerf_open(&dev, NULL);
    if (status != 0) {
        fprintf(stderr, "open: %s\n", bladerf_strerror(status));
        return 1;
    }

    status = 0;
    if (config.enable_rx1) {
        status = bladerf_enable_module(dev, BLADERF_CHANNEL_RX(0), true);
    }
    if (status == 0 && config.enable_rx2) {
        status = bladerf_enable_module(dev, BLADERF_CHANNEL_RX(1), true);
    }
    if (status == 0) {
        status = bladerf_sync_config(dev, config.layout,
                                     BLADERF_FORMAT_SC16_Q11_META,
                                     8, config.sync_samples, 4, 1000);
    }
    if (status == 0) {
        status = assert_legacy_setter_blocked_during_transition(dev, &config);
    }
    for (size_t i = 0; status == 0 && i < sizeof(stages) / sizeof(stages[0]); ++i) {
        struct bladerf_rf_event failed = {0};
        struct bladerf_rf_event recovered = {0};
        status = transition(dev, &config, true, stages[i], &failed);
        if (status == 0) {
            status = assert_sync_withheld(dev, &config);
        }
        if (status == 0) {
            status = transition(dev, &config, false, NULL, &recovered);
        }
        if (status == 0) {
            status = assert_sync_valid(dev, &config, &recovered);
        }
        if (status == 0) {
            printf("%s fail-closed recovery: PASS epoch=%u\n",
                   stages[i], recovered.epoch_id);
        }
    }

    if (config.enable_rx1) {
        (void)bladerf_enable_module(dev, BLADERF_CHANNEL_RX(0), false);
    }
    if (config.enable_rx2) {
        (void)bladerf_enable_module(dev, BLADERF_CHANNEL_RX(1), false);
    }
    bladerf_close(dev);
    if (status != 0) {
        fprintf(stderr, "positive-timeout qualification failed: %s\n",
                bladerf_strerror(status));
        return 1;
    }
    printf("RX positive-timeout qualification: PASS (%s, RFPLL/ENSM/BBPLL/FPGA)\n",
           config.name);
    return 0;
}
