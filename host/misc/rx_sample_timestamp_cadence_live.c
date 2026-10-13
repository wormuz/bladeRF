#include <libbladeRF.h>

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t monotonic_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts) != 0) {
        perror("clock_gettime");
        exit(EXIT_FAILURE);
    }
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) +
           (uint64_t)ts.tv_nsec;
}

static int parse_uint(const char *text, unsigned long *value)
{
    char *end = NULL;
    errno = 0;
    *value = strtoul(text, &end, 10);
    return errno == 0 && end != text && *end == '\0' ? 0 : -1;
}

int main(int argc, char **argv)
{
    bladerf_sample_rate rate = 23040000;
    unsigned blocks = 512;
    unsigned block_samples = 65536;
    const char *layout_name = argc > 1 ? argv[1] : "RX1";
    bladerf_channel_layout layout;
    bladerf_channel channel;
    bool paired;
    struct bladerf *dev = NULL;
    int16_t *samples = NULL;
    uint32_t transaction_id = 0;
    struct bladerf_rf_event event = {0};
    struct bladerf_metadata meta = {0};
    uint64_t first_timestamp = 0, expected_timestamp = 0;
    uint64_t start_ns = 0, end_ns = 0;
    uint64_t previous_timestamp = 0;
    unsigned count_per_timestamp;
    unsigned requested_count;
    int status = 0;
    unsigned completed = 0;

    if (argc > 2) {
        unsigned long parsed;
        if (parse_uint(argv[2], &parsed) != 0 || parsed == 0 ||
            parsed > UINT32_MAX) {
            fprintf(stderr, "invalid sample rate: %s\n", argv[2]);
            return EXIT_FAILURE;
        }
        rate = (bladerf_sample_rate)parsed;
    }
    if (argc > 3) {
        unsigned long parsed;
        if (parse_uint(argv[3], &parsed) != 0 || parsed == 0 || parsed > 4096) {
            fprintf(stderr, "invalid block count: %s\n", argv[3]);
            return EXIT_FAILURE;
        }
        blocks = (unsigned)parsed;
    }
    if (argc > 4) {
        unsigned long parsed;
        if (parse_uint(argv[4], &parsed) != 0 || parsed < 1024 ||
            parsed > 1048576) {
            fprintf(stderr, "invalid block sample count: %s\n", argv[4]);
            return EXIT_FAILURE;
        }
        block_samples = (unsigned)parsed;
    }

    if (strcmp(layout_name, "RX1") == 0) {
        layout = BLADERF_RX_X1;
        channel = BLADERF_CHANNEL_RX(0);
        paired = false;
    } else if (strcmp(layout_name, "RX2") == 0) {
        layout = BLADERF_RX_X1;
        channel = BLADERF_CHANNEL_RX(1);
        paired = false;
    } else if (strcmp(layout_name, "RX_X2") == 0) {
        layout = BLADERF_RX_X2;
        channel = BLADERF_CHANNEL_RX(0);
        paired = true;
    } else {
        fprintf(stderr, "usage: %s [RX1|RX2|RX_X2] [rate_hz] [blocks] [samples_per_lane]\n",
                argv[0]);
        return EXIT_FAILURE;
    }

    count_per_timestamp = paired ? 2u : 1u;
    if (block_samples > UINT32_MAX / count_per_timestamp) {
        fprintf(stderr, "requested block is too large\n");
        return EXIT_FAILURE;
    }
    requested_count = block_samples * count_per_timestamp;
    samples = calloc((size_t)requested_count * 2u, sizeof(*samples));
    if (samples == NULL) {
        perror("calloc");
        return EXIT_FAILURE;
    }

    status = bladerf_open(&dev, NULL);
    if (status != 0) {
        fprintf(stderr, "bladerf_open: %s\n", bladerf_strerror(status));
        goto out;
    }
    status = bladerf_set_sample_rate(dev, channel, rate, NULL);
    if (status != 0) {
        fprintf(stderr, "set_sample_rate: %s\n", bladerf_strerror(status));
        goto out;
    }
    status = bladerf_set_bandwidth(dev, channel, 20000000, NULL);
    if (status != 0) {
        fprintf(stderr, "set_bandwidth: %s\n", bladerf_strerror(status));
        goto out;
    }
    status = bladerf_set_gain(dev, channel, 30);
    if (status != 0) {
        fprintf(stderr, "set_gain: %s\n", bladerf_strerror(status));
        goto out;
    }
    status = bladerf_sync_config(dev, layout, BLADERF_FORMAT_SC16_Q11_META,
                                 32, block_samples, 8, 5000);
    if (status != 0) {
        fprintf(stderr, "sync_config: %s\n", bladerf_strerror(status));
        goto out;
    }
    status = bladerf_enable_module(dev, channel, true);
    if (status != 0) {
        fprintf(stderr, "enable RX: %s\n", bladerf_strerror(status));
        goto out;
    }
    if (paired) {
        status = bladerf_enable_module(dev, BLADERF_CHANNEL_RX(1), true);
        if (status != 0) {
            fprintf(stderr, "enable RX2: %s\n", bladerf_strerror(status));
            goto out;
        }
    }

    struct bladerf_rx_transition_request request = {
        .target_frequency_hz = 1835000000ULL,
        .required_events_mask = BLADERF_RF_REQUIRE_PLL_LOCKED |
                                BLADERF_RF_REQUIRE_ENSM_RX |
                                BLADERF_RF_REQUIRE_EPOCH_VALID,
        .timeout_ms = 2000,
        .require_rx_data_valid = true,
        .epoch_settle_samples = 0,
    };
    status = bladerf_rx_transition_begin(dev, channel, &request,
                                         &transaction_id);
    if (status != 0) {
        fprintf(stderr, "transition_begin: %s\n", bladerf_strerror(status));
        goto out;
    }
    status = bladerf_rx_transition_wait(dev, transaction_id, &event, 3000);
    if (status != 0) {
        fprintf(stderr, "transition_wait: %s\n", bladerf_strerror(status));
        goto out;
    }

    printf("START layout=%s requested_rate=%" PRIu64 " epoch=%u blocks=%u "
           "samples_per_lane=%u samples_per_timestamp=%u\n",
           layout_name, (uint64_t)rate, event.epoch_id, blocks,
           block_samples, count_per_timestamp);

    for (unsigned i = 0; i < blocks; ++i) {
        meta = (struct bladerf_metadata){0};
        meta.flags = BLADERF_META_FLAG_RX_NOW;
        if (i == 0) start_ns = monotonic_ns();
        status = bladerf_sync_rx(dev, samples, requested_count, &meta, 5000);
        if (status != 0 || meta.actual_count != requested_count ||
            !meta.rx_epoch_id_valid || meta.rx_epoch_id != event.epoch_id ||
            (meta.status & BLADERF_META_STATUS_OVERRUN) != 0) {
            fprintf(stderr, "BLOCK_INVALID i=%u status=%s count=%u/%u "
                    "epoch=%u/%u epoch_valid=%u meta_status=0x%x ts=%" PRIu64 "\n",
                    i, bladerf_strerror(status), meta.actual_count,
                    requested_count, meta.rx_epoch_id, event.epoch_id,
                    meta.rx_epoch_id_valid, meta.status,
                    (uint64_t)meta.timestamp);
            status = status != 0 ? status : BLADERF_ERR_UNEXPECTED;
            goto out;
        }
        uint64_t timestamp_step = meta.actual_count / count_per_timestamp;
        if (meta.actual_count % count_per_timestamp != 0) {
            fprintf(stderr, "BLOCK_ODD_INTERLEAVED_COUNT i=%u count=%u\n",
                    i, meta.actual_count);
            status = BLADERF_ERR_UNEXPECTED;
            goto out;
        }
        if (i == 0) {
            first_timestamp = meta.timestamp;
        } else if (meta.timestamp != expected_timestamp) {
            fprintf(stderr, "TIMESTAMP_SAMPLE_COUNT_MISMATCH i=%u got=%" PRIu64
                    " expected=%" PRIu64 " delta=%" PRId64 "\n",
                    i, (uint64_t)meta.timestamp, expected_timestamp,
                    (int64_t)(meta.timestamp - expected_timestamp));
            status = BLADERF_ERR_UNEXPECTED;
            goto out;
        }
        previous_timestamp = meta.timestamp;
        expected_timestamp = meta.timestamp + timestamp_step;
        ++completed;
    }
    end_ns = monotonic_ns();
    {
        double elapsed_s = (double)(end_ns - start_ns) / 1e9;
        uint64_t total_ticks = expected_timestamp - first_timestamp;
        double measured_rate = (double)total_ticks / elapsed_s;
        printf("PASS layout=%s blocks=%u samples_per_lane=%" PRIu64
               " timestamp_ticks=%" PRIu64 " elapsed_s=%.9f "
               "timestamp_rate_hz=%.3f requested_rate_hz=%" PRIu64
               " first_ts=%" PRIu64 " last_block_ts=%" PRIu64 "\n",
               layout_name, completed,
               (uint64_t)completed * block_samples,
               total_ticks, elapsed_s, measured_rate, (uint64_t)rate,
               first_timestamp, previous_timestamp);
    }

out:
    if (dev != NULL) {
        if (paired) (void)bladerf_enable_module(dev, BLADERF_CHANNEL_RX(1), false);
        (void)bladerf_enable_module(dev, channel, false);
        bladerf_close(dev);
    }
    free(samples);
    return status == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
