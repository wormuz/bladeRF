#define _DEFAULT_SOURCE

/* xA4 host-side regression for deferred NIOS RX fastlock failure visibility.
 * Build with BLADERF_ENABLE_TEST_RX_TRANSITION_STALL_INJECTION and run with
 * the matching local libbladeRF. The monitor injects one counter increment
 * only after a future RX retune has been accepted. */
#include <libbladeRF.h>

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RX_CHANNEL BLADERF_CHANNEL_RX(0)
#define RX_RATE 1000000u
#define RX_BANDWIDTH 1500000u
#define FREQ_A 1835000000ULL
#define FREQ_B 1835100000ULL
#define EVENT_CAPACITY BLADERF_RF_EVENT_HISTORY_SIZE

int main(void)
{
    struct bladerf *dev = NULL;
    struct bladerf_quick_tune quick_tune[2];
    struct bladerf_rf_event event;
    struct bladerf_rf_event events[EVENT_CAPACITY];
    struct bladerf_metadata metadata = { .flags = BLADERF_META_FLAG_RX_NOW };
    int16_t samples[8192 * 2] = {0};
    uint32_t transaction_id = 0;
    uint32_t event_count = 0;
    uint64_t cursor = 0;
    uint64_t next_cursor = 0;
    uint64_t now = 0;
    bool history_complete = false;
    bool saw_invalidation = false;
    bool saw_failure = false;
    int status;

#define CHECK(call, label) do { \
        status = (call); \
        if (status != 0) { \
            fprintf(stderr, "%s: %s (%d)\n", label, \
                    bladerf_strerror(status), status); \
            goto done; \
        } \
    } while (0)

    CHECK(bladerf_open(&dev, NULL), "open");
    CHECK(bladerf_set_sample_rate(dev, RX_CHANNEL, RX_RATE, NULL),
          "sample rate");
    CHECK(bladerf_set_bandwidth(dev, RX_CHANNEL, RX_BANDWIDTH, NULL),
          "bandwidth");
    CHECK(bladerf_set_frequency(dev, RX_CHANNEL, FREQ_A), "tune A");
    CHECK(bladerf_get_quick_tune(dev, RX_CHANNEL, &quick_tune[0]),
          "profile A");
    CHECK(bladerf_set_frequency(dev, RX_CHANNEL, FREQ_B), "tune B");
    CHECK(bladerf_get_quick_tune(dev, RX_CHANNEL, &quick_tune[1]),
          "profile B");
    CHECK(bladerf_set_frequency(dev, RX_CHANNEL, FREQ_A), "restore A");
    CHECK(bladerf_sync_config(dev, BLADERF_RX_X1,
                              BLADERF_FORMAT_SC16_Q11_META,
                              16, 8192, 8, 1000), "sync config");
    CHECK(bladerf_enable_module(dev, RX_CHANNEL, true), "enable RX");

    struct bladerf_rx_transition_request request = {
        .target_frequency_hz = FREQ_A,
        .required_events_mask = BLADERF_RF_REQUIRE_PLL_LOCKED,
        .timeout_ms = 2000,
        .require_rx_data_valid = true,
        .epoch_settle_samples = 0,
    };
    CHECK(bladerf_rx_transition_begin(dev, RX_CHANNEL, &request,
                                      &transaction_id), "transition begin");
    CHECK(bladerf_rx_transition_wait(dev, transaction_id, &event, 2000),
          "transition wait");
    if (event.event_type != BLADERF_RF_EVT_RX_EPOCH_VALID) {
        fprintf(stderr, "initial RX epoch was not certified: event=%u\n",
                event.event_type);
        status = BLADERF_ERR_UNEXPECTED;
        goto done;
    }
    CHECK(bladerf_rf_events_get_since(dev, 0, events, EVENT_CAPACITY,
                                      &event_count, &cursor,
                                      &history_complete), "event cursor");
    if (!history_complete) {
        status = BLADERF_ERR_UNEXPECTED;
        goto done;
    }
    CHECK(bladerf_get_timestamp(dev, BLADERF_RX, &now), "RX timestamp");
    CHECK(setenv("BLADERF_TEST_RX_TRANSITION_STALL",
                 "RUNTIME_RX_RETUNE_FAILURE", 1) == 0
              ? 0 : BLADERF_ERR_UNEXPECTED,
          "enable failure injection");
    CHECK(bladerf_schedule_retune(dev, RX_CHANNEL,
                                 now + 10ULL * RX_RATE, FREQ_B,
                                 &quick_tune[1]),
          "schedule future RX retune");

    for (unsigned attempt = 0; attempt < 3000 && !saw_failure; ++attempt) {
        usleep(1000);
        CHECK(bladerf_rf_events_get_since(
                  dev, cursor, events, EVENT_CAPACITY, &event_count,
                  &next_cursor, &history_complete), "poll RX events");
        if (!history_complete) {
            status = BLADERF_ERR_UNEXPECTED;
            goto done;
        }
        cursor = next_cursor;
        for (uint32_t i = 0; i < event_count; ++i) {
            if (events[i].event_type == BLADERF_RF_EVT_RX_DATA_INVALIDATED) {
                saw_invalidation = true;
            }
            if (events[i].event_type == BLADERF_RF_EVT_RX_RETUNE_FAILED &&
                events[i].error_code == BLADERF_ERR_FPGA_OP &&
                events[i].flags != 0 && saw_invalidation) {
                saw_failure = true;
                break;
            }
        }
    }
    if (!saw_failure) {
        fputs("deferred RX retune failure event not observed\n", stderr);
        status = BLADERF_ERR_UNEXPECTED;
        goto done;
    }

    metadata = (struct bladerf_metadata){
        .flags = BLADERF_META_FLAG_RX_NOW,
    };
    status = bladerf_sync_rx(dev, samples, 8192, &metadata, 100);
    if (status == 0 || metadata.actual_count != 0) {
        fprintf(stderr, "RX IQ delivered after retune failure: status=%d "
                "count=%u\n", status, metadata.actual_count);
        status = BLADERF_ERR_UNEXPECTED;
        goto done;
    }
    printf("deferred RX retune failure reported; IQ remains withheld\n");
    status = 0;

done:
    unsetenv("BLADERF_TEST_RX_TRANSITION_STALL");
    if (dev != NULL) {
        (void)bladerf_enable_module(dev, RX_CHANNEL, false);
        bladerf_close(dev);
    }
    return status == 0 ? 0 : 1;
}
