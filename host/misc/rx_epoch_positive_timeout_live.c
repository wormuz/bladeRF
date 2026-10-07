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

static uint64_t monotonic_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int transition(struct bladerf *dev, bool inject_stall,
                      const char *stage, struct bladerf_rf_event *event)
{
    const bladerf_channel ch = BLADERF_CHANNEL_RX(1);
    struct bladerf_rx_transition_request request = {0};
    uint32_t transaction_id = 0;
    int status;

    request.target_frequency_hz = 1835000000ULL;
    request.required_events_mask = BLADERF_RF_REQUIRE_EPOCH_VALID;
    request.require_rx_data_valid = true;
    request.timeout_ms = 2000;
    status = bladerf_rx_transition_begin(dev, ch, &request, &transaction_id);
    if (status != 0) {
        return status;
    }

    if (inject_stall && setenv("BLADERF_TEST_RX_TRANSITION_STALL", stage, 1) != 0) {
        return BLADERF_ERR_UNEXPECTED;
    }
    const uint64_t started_ns = monotonic_ns();
    status = bladerf_rx_transition_wait(dev, transaction_id, event,
                                        inject_stall ? WAIT_TIMEOUT_MS : 2000);
    const uint64_t elapsed_ns = monotonic_ns() - started_ns;
    if (inject_stall) {
        unsetenv("BLADERF_TEST_RX_TRANSITION_STALL");
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

static int assert_sync_withheld(struct bladerf *dev)
{
    int16_t samples[SAMPLE_COUNT * 2];
    int16_t sentinel[SAMPLE_COUNT * 2];
    struct bladerf_metadata metadata = {0};
    memset(samples, 0x5a, sizeof(samples));
    memcpy(sentinel, samples, sizeof(sentinel));
    metadata.flags = BLADERF_META_FLAG_RX_NOW;

    const int status = bladerf_sync_rx(dev, samples, SAMPLE_COUNT,
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
                             const struct bladerf_rf_event *event)
{
    int16_t samples[SAMPLE_COUNT * 2];
    uint64_t expected_timestamp = event->fpga_timestamp;
    unsigned int total = 0;
    for (unsigned int attempt = 0; total < SAMPLE_COUNT && attempt < 30;
         ++attempt) {
        struct bladerf_metadata metadata = {0};
        metadata.flags = BLADERF_META_FLAG_RX_NOW;
        const int status = bladerf_sync_rx(
            dev, samples + 2 * total, SAMPLE_COUNT - total, &metadata, 200);
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
        expected_timestamp = metadata.timestamp + metadata.actual_count;
    }
    if (total != SAMPLE_COUNT) {
        fprintf(stderr, "recovery block incomplete: received=%u expected=%u\n",
                total, SAMPLE_COUNT);
        return BLADERF_ERR_UNEXPECTED;
    }
    return 0;
}

int main(void)
{
    static const char *const stages[] = {"PLL", "ENSM", "EPOCH"};
    const bladerf_channel ch = BLADERF_CHANNEL_RX(1);
    struct bladerf *dev = NULL;
    int status = bladerf_open(&dev, NULL);
    if (status != 0) {
        fprintf(stderr, "open: %s\n", bladerf_strerror(status));
        return 1;
    }

    status = bladerf_enable_module(dev, ch, true);
    if (status == 0) {
        status = bladerf_sync_config(dev, BLADERF_RX_X1,
                                     BLADERF_FORMAT_SC16_Q11_META,
                                     8, SAMPLE_COUNT, 4, 1000);
    }
    for (size_t i = 0; status == 0 && i < sizeof(stages) / sizeof(stages[0]); ++i) {
        struct bladerf_rf_event failed = {0};
        struct bladerf_rf_event recovered = {0};
        status = transition(dev, true, stages[i], &failed);
        if (status == 0) {
            status = assert_sync_withheld(dev);
        }
        if (status == 0) {
            status = transition(dev, false, NULL, &recovered);
        }
        if (status == 0) {
            status = assert_sync_valid(dev, &recovered);
        }
        if (status == 0) {
            printf("%s fail-closed recovery: PASS epoch=%u\n",
                   stages[i], recovered.epoch_id);
        }
    }

    (void)bladerf_enable_module(dev, ch, false);
    bladerf_close(dev);
    if (status != 0) {
        fprintf(stderr, "positive-timeout qualification failed: %s\n",
                bladerf_strerror(status));
        return 1;
    }
    printf("RX positive-timeout qualification: PASS (RX2, PLL/ENSM/FPGA)\n");
    return 0;
}
