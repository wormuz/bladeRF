#include <libbladeRF.h>

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bladerf_channel rx_channel = BLADERF_CHANNEL_RX(0);
#define RX_RATE 1920000
#define RX_BANDWIDTH 1500000
#define RX_READ_SAMPLES 8192
#define FREQ_A 1835000000ULL
#define FREQ_B 1835400000ULL

static int fail(const char *what, int status)
{
    fprintf(stderr, "%s: %s (%d)\n", what, bladerf_strerror(status), status);
    return 1;
}

static int find_event(const struct bladerf_rf_event *events, uint32_t count,
                      bladerf_rf_event_type type,
                      struct bladerf_rf_event *result)
{
    for (uint32_t i = 0; i < count; ++i) {
        if (events[i].event_type == type) {
            *result = events[i];
            return 0;
        }
    }
    return -1;
}

int main(int argc, char **argv)
{
    const unsigned trials = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 10) : 20;
    const unsigned buffer_samples = argc > 2 ?
        (unsigned)strtoul(argv[2], NULL, 10) : 131072;
    const unsigned transfers = argc > 3 ?
        (unsigned)strtoul(argv[3], NULL, 10) : 2;
    const unsigned rx_index = argc > 4 ? (unsigned)strtoul(argv[4], NULL, 10) : 0;
    const bladerf_channel_layout layout = argc > 5 && strcmp(argv[5], "x2") == 0
        ? BLADERF_RX_X2 : BLADERF_RX_X1;
    const unsigned read_samples = argc > 6 ?
        (unsigned)strtoul(argv[6], NULL, 10) :
        (buffer_samples < RX_READ_SAMPLES ? buffer_samples : RX_READ_SAMPLES);
    const unsigned buffers = transfers * 2 < 16 ? 16 : transfers * 2;
    struct bladerf *dev = NULL;
    struct bladerf_quick_tune profiles[2];
    int16_t *samples = NULL;
    int status;
    int result = 1;

    if (trials == 0 || buffer_samples < 1024 || transfers == 0 ||
        transfers > 32 ||
        buffer_samples % 1024 != 0 || read_samples == 0) {
        fputs("usage: rx_epoch_host_handoff_live [trials] [buffer_samples] "
              "[transfers] [rx_channel:0|1] [x1|x2] [read_samples]\n",
              stderr);
        return 2;
    }
    if (rx_index > 1 ||
        (argc > 5 && strcmp(argv[5], "x1") != 0 &&
         strcmp(argv[5], "x2") != 0)) {
        fputs("RX channel must be 0 or 1; layout must be x1 or x2\n", stderr);
        return 2;
    }
    rx_channel = BLADERF_CHANNEL_RX(rx_index);

    status = bladerf_open(&dev, NULL);
    if (status != 0) return fail("open", status);
    if (getenv("BLADERF_TEST_RX_EPOCH_DEBUG") != NULL) {
        bladerf_log_set_verbosity(BLADERF_LOG_LEVEL_DEBUG);
    }

#define CHECK(call, name) do { \
        status = (call); \
        if (status != 0) { fail((name), status); goto done; } \
    } while (0)
    CHECK(bladerf_set_sample_rate(dev, rx_channel, RX_RATE, NULL),
          "set sample rate");
    CHECK(bladerf_set_bandwidth(dev, rx_channel, RX_BANDWIDTH, NULL),
          "set bandwidth");
    CHECK(bladerf_set_frequency(dev, rx_channel, FREQ_A), "set LO A");
    CHECK(bladerf_get_quick_tune(dev, rx_channel, &profiles[0]),
          "get quick tune A");
    CHECK(bladerf_set_frequency(dev, rx_channel, FREQ_B), "set LO B");
    CHECK(bladerf_get_quick_tune(dev, rx_channel, &profiles[1]),
          "get quick tune B");
    CHECK(bladerf_set_frequency(dev, rx_channel, FREQ_A), "restore LO A");
    CHECK(bladerf_sync_config(dev, layout,
          BLADERF_FORMAT_SC16_Q11_META, buffers, buffer_samples,
          transfers, 1000),
          "sync config");
    CHECK(bladerf_enable_module(dev, rx_channel, true), "enable RX");
#undef CHECK

    samples = calloc((size_t)read_samples * 2, sizeof(*samples));
    if (samples == NULL) {
        fputs("sample allocation failed\n", stderr);
        goto done;
    }

    /* Establish a certified epoch before reading: sync RX intentionally
     * blocks unqualified META IQ after this API is enabled. */
    {
        const struct bladerf_rx_transition_request request = {
            .target_frequency_hz = FREQ_A,
            .required_events_mask = BLADERF_RF_REQUIRE_PLL_LOCKED |
                                    BLADERF_RF_REQUIRE_ENSM_RX |
                                    BLADERF_RF_REQUIRE_EPOCH_VALID,
            .timeout_ms = 2000,
            .require_rx_data_valid = true,
            .epoch_settle_samples = 0,
        };
        struct bladerf_rf_event event = {0};
        uint32_t transaction = 0;
        struct bladerf_metadata metadata;
        bool warmed = false;
        status = bladerf_rx_transition_begin_quick_tune(
            dev, rx_channel, &request, &profiles[0], &transaction);
        if (status == 0) {
            status = bladerf_rx_transition_wait(dev, transaction, &event,
                                                request.timeout_ms);
        }
        if (status != 0) {
            fail("initial epoch", status);
            goto done;
        }
        for (unsigned attempt = 0; attempt < 16 && !warmed; ++attempt) {
            metadata = (struct bladerf_metadata){0};
            metadata.flags = BLADERF_META_FLAG_RX_NOW;
            status = bladerf_sync_rx(dev, samples, read_samples, &metadata, 2000);
            if (status == 0 && metadata.actual_count == read_samples &&
                metadata.rx_epoch_id_valid && metadata.rx_epoch_id == event.epoch_id &&
                !(metadata.status & BLADERF_META_STATUS_OVERRUN)) {
                warmed = true;
            }
        }
        if (!warmed) {
            fprintf(stderr, "warm-up failed to reach clean RX: status=%s count=%u "
                    "epoch=%u/%u valid=%u meta_status=0x%x ts=%" PRIu64 "\n",
                    bladerf_strerror(status), metadata.actual_count,
                    metadata.rx_epoch_id, event.epoch_id,
                    metadata.rx_epoch_id_valid, metadata.status, metadata.timestamp);
            fail("warm-up RX", status ? status : BLADERF_ERR_UNEXPECTED);
            goto done;
        }
    }

    printf("host-data handoff trials=%u channel=%u layout=%s rate=%u "
           "buffer=%u buffers=%u transfers=%u read=%u\n", trials, rx_index,
           layout == BLADERF_RX_X2 ? "x2" : "x1", RX_RATE, buffer_samples,
           buffers, transfers, read_samples);

    for (unsigned i = 0; i < trials; ++i) {
        const unsigned target = (i + 1) & 1u;
        const struct bladerf_rx_transition_request request = {
            .target_frequency_hz = target ? FREQ_B : FREQ_A,
            .required_events_mask = BLADERF_RF_REQUIRE_PLL_LOCKED |
                                    BLADERF_RF_REQUIRE_ENSM_RX |
                                    BLADERF_RF_REQUIRE_EPOCH_VALID,
            .timeout_ms = 2000,
            .require_rx_data_valid = true,
            .epoch_settle_samples = 0,
        };
        struct bladerf_rf_event final_event = {0};
        struct bladerf_rf_event events[BLADERF_RF_EVENT_HISTORY_SIZE];
        struct bladerf_rf_event epoch_event = {0};
        struct bladerf_rf_event host_event = {0};
        struct bladerf_metadata metadata = {0};
        uint32_t transaction = 0, count = 0;
        bool complete = false;
        bool clean_read = false;

        status = bladerf_rx_transition_begin_quick_tune(
            dev, rx_channel, &request, &profiles[target], &transaction);
        if (status == 0) {
            status = bladerf_rx_transition_wait(dev, transaction, &final_event,
                                                request.timeout_ms);
        }
        if (status != 0) {
            fail("transition", status);
            goto done;
        }

        for (unsigned attempt = 0; attempt < 16; ++attempt) {
            metadata = (struct bladerf_metadata){0};
            metadata.flags = BLADERF_META_FLAG_RX_NOW;
            status = bladerf_sync_rx(dev, samples, read_samples, &metadata,
                                     2000);
            if (status == BLADERF_ERR_WOULD_BLOCK) {
                continue;
            }
            if (status != 0) {
                break;
            }
            if (metadata.actual_count == read_samples &&
                !(metadata.status & BLADERF_META_STATUS_OVERRUN) &&
                metadata.rx_epoch_id_valid &&
                metadata.rx_epoch_id == final_event.epoch_id &&
                metadata.timestamp >= final_event.fpga_timestamp) {
                clean_read = true;
                break;
            }
        }
        if (status != 0 || !clean_read) {
            if (status == 0) {
                fprintf(stderr, "no clean read after 16 metadata-checked "
                        "attempts: count=%u/%u status=0x%x epoch=%u/%u "
                        "valid=%u timestamp=%" PRIu64 " boundary=%" PRIu64 "\n",
                        metadata.actual_count, read_samples, metadata.status,
                        metadata.rx_epoch_id, final_event.epoch_id,
                        metadata.rx_epoch_id_valid, metadata.timestamp,
                        final_event.fpga_timestamp);
                status = BLADERF_ERR_UNEXPECTED;
            }
            fail("first sync RX", status);
            goto done;
        }
        status = bladerf_rx_transition_get_events(
            dev, transaction, events, BLADERF_RF_EVENT_HISTORY_SIZE,
            &count, &complete);
        if (status != 0 || !complete ||
            find_event(events, count, BLADERF_RF_EVT_RX_EPOCH_VALID,
                       &epoch_event) != 0 ||
            find_event(events, count, BLADERF_RF_EVT_RX_FIRST_VALID_HOST_DATA,
                       &host_event) != 0) {
            fprintf(stderr, "missing causal event trial=%u txn=%u status=%s "
                    "complete=%u events=%u count=%u meta=0x%x epoch=%u "
                    "valid=%u timestamp=%" PRIu64 "\n", i, transaction,
                    bladerf_strerror(status), complete, count,
                    metadata.actual_count, metadata.status,
                    metadata.rx_epoch_id, metadata.rx_epoch_id_valid,
                    metadata.timestamp);
            goto done;
        }
        if (metadata.actual_count != read_samples ||
            metadata.status & BLADERF_META_STATUS_OVERRUN ||
            !metadata.rx_epoch_id_valid ||
            metadata.rx_epoch_id != final_event.epoch_id ||
            metadata.timestamp < epoch_event.fpga_timestamp ||
            host_event.transaction_id != transaction ||
            host_event.epoch_id != final_event.epoch_id) {
            fprintf(stderr, "invalid host IQ trial=%u txn=%u count=%u "
                    "status=0x%x epoch=%u/%u valid=%u timestamp=%" PRIu64
                    " boundary=%" PRIu64 "\n", i, transaction,
                    metadata.actual_count, metadata.status,
                    metadata.rx_epoch_id, final_event.epoch_id,
                    metadata.rx_epoch_id_valid, metadata.timestamp,
                    epoch_event.fpga_timestamp);
            goto done;
        }

        printf("trial=%u txn=%u epoch=%u wait_to_first_iq_us=%.3f "
               "timestamp_delta_samples=%" PRIu64 " count=%u status=0x%x\n",
               i, transaction, final_event.epoch_id,
               (host_event.host_monotonic_ns -
                epoch_event.host_monotonic_ns) / 1000.0,
               metadata.timestamp - epoch_event.fpga_timestamp,
               metadata.actual_count, metadata.status);
    }

    result = 0;

done:
    if (dev != NULL) (void)bladerf_enable_module(dev, rx_channel, false);
    free(samples);
    bladerf_close(dev);
    return result;
}
