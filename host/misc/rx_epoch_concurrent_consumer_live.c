#include <libbladeRF.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RX_BLOCK_SAMPLES 8192
#define RX_BUFFER_SAMPLES_DEFAULT 8192

struct reader_state {
    struct bladerf *dev;
    atomic_bool running;
    atomic_uint blocks;
    atomic_uint would_block;
    atomic_uint timeouts;
    atomic_uint invalid_blocks;
    atomic_uint overruns;
    atomic_uint bad_timestamps;
};

static void *reader_thread(void *arg)
{
    struct reader_state *state = arg;
    int16_t *samples = calloc(RX_BLOCK_SAMPLES * 2, sizeof(*samples));
    uint64_t last_timestamp = 0;

    if (samples == NULL) {
        atomic_store(&state->invalid_blocks, 1);
        return NULL;
    }

    while (atomic_load(&state->running)) {
        struct bladerf_metadata meta = {0};
        meta.flags = BLADERF_META_FLAG_RX_NOW;
        int status = bladerf_sync_rx(state->dev, samples, RX_BLOCK_SAMPLES,
                                     &meta, 100);
        if (status == BLADERF_ERR_WOULD_BLOCK) {
            atomic_fetch_add(&state->would_block, 1);
            /* The transition thread owns recovery. Avoid a tight retry loop
             * while the epoch is deliberately fenced. */
            usleep(1000);
            continue;
        }
        if (status == BLADERF_ERR_TIMEOUT) {
            atomic_fetch_add(&state->timeouts, 1);
            continue;
        }
        if (status != 0 || !meta.rx_epoch_id_valid ||
            meta.actual_count != RX_BLOCK_SAMPLES) {
            atomic_fetch_add(&state->invalid_blocks, 1);
            continue;
        }
        if (meta.status & BLADERF_META_STATUS_OVERRUN) {
            atomic_fetch_add(&state->overruns, 1);
        }
        if (last_timestamp != 0 && meta.timestamp <= last_timestamp) {
            atomic_fetch_add(&state->bad_timestamps, 1);
        }
        last_timestamp = meta.timestamp;
        atomic_fetch_add(&state->blocks, 1);
    }

    free(samples);
    return NULL;
}

static int collect_overruns(struct bladerf *dev, uint64_t *cursor,
                            uint32_t *overruns)
{
    struct bladerf_rf_event events[BLADERF_RF_EVENT_HISTORY_SIZE];
    uint32_t count = 0;
    uint64_t next = *cursor;
    bool complete = false;
    int status = bladerf_rf_events_get_since(
        dev, *cursor, events, BLADERF_RF_EVENT_HISTORY_SIZE,
        &count, &next, &complete);
    if (status != 0) return status;
    if (!complete) {
        fprintf(stderr, "event history gap cursor=%llu next=%llu\n",
                (unsigned long long)*cursor, (unsigned long long)next);
        return BLADERF_ERR_UNEXPECTED;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (events[i].event_type == BLADERF_RF_EVT_RX_STREAM_OVERRUN) {
            ++*overruns;
            fprintf(stderr, "RX_X2 overrun epoch=%u flags=0x%x\n",
                    events[i].epoch_id, events[i].flags);
        }
    }
    *cursor = next;
    return 0;
}

int main(int argc, char **argv)
{
    unsigned transitions = 1000;
    unsigned dwell_ms = 0;
    unsigned sample_rate = 4000000;
    unsigned buffer_samples = RX_BUFFER_SAMPLES_DEFAULT;
    if (argc > 1) {
        char *end = NULL;
        unsigned long parsed = strtoul(argv[1], &end, 10);
        if (end == argv[1] || *end != '\0' || parsed == 0 ||
            parsed > UINT32_MAX) {
            fprintf(stderr, "usage: %s [transition_count]\n", argv[0]);
            return 2;
        }
        transitions = (unsigned)parsed;
    }
    if (argc > 2) {
        char *end = NULL;
        unsigned long parsed = strtoul(argv[2], &end, 10);
        if (end == argv[2] || *end != '\0' || parsed > 60000) {
            fprintf(stderr, "usage: %s [transition_count] [dwell_ms] [sample_rate] [buffer_samples]\n", argv[0]);
            return 2;
        }
        dwell_ms = (unsigned)parsed;
    }
    if (argc > 3) {
        char *end = NULL;
        unsigned long parsed = strtoul(argv[3], &end, 10);
        if (end == argv[3] || *end != '\0' || parsed == 0 ||
            parsed > 10000000) {
            fprintf(stderr, "usage: %s [transition_count] [dwell_ms] [sample_rate]\n", argv[0]);
            return 2;
        }
        sample_rate = (unsigned)parsed;
    }
    if (argc > 4) {
        char *end = NULL;
        unsigned long parsed = strtoul(argv[4], &end, 10);
        if (end == argv[4] || *end != '\0' || parsed < RX_BLOCK_SAMPLES ||
            parsed > 262144 || parsed % RX_BLOCK_SAMPLES != 0) {
            fprintf(stderr, "usage: %s [transition_count] [dwell_ms] [sample_rate] [buffer_samples]\n", argv[0]);
            return 2;
        }
        buffer_samples = (unsigned)parsed;
    }

    struct bladerf *dev = NULL;
    int status = bladerf_open(&dev, NULL);
    if (status != 0) {
        fprintf(stderr, "open: %s\n", bladerf_strerror(status));
        return 2;
    }
    bladerf_log_set_verbosity(BLADERF_LOG_LEVEL_CRITICAL);

    status = bladerf_set_sample_rate(dev, BLADERF_CHANNEL_RX(0), sample_rate,
                                     NULL);
    if (status == 0) status = bladerf_set_bandwidth(
        dev, BLADERF_CHANNEL_RX(0), 5000000, NULL);
    if (status == 0) status = bladerf_set_gain(
        dev, BLADERF_CHANNEL_RX(0), 30);
    if (status == 0) status = bladerf_set_gain(
        dev, BLADERF_CHANNEL_RX(1), 30);
    if (status == 0) status = bladerf_sync_config(
        dev, BLADERF_RX_X2, BLADERF_FORMAT_SC16_Q11_META,
        16, buffer_samples, 8, 1000);
    if (status == 0) status = bladerf_enable_module(
        dev, BLADERF_CHANNEL_RX(0), true);
    if (status == 0) status = bladerf_enable_module(
        dev, BLADERF_CHANNEL_RX(1), true);
    if (status != 0) {
        fprintf(stderr, "setup: %s\n", bladerf_strerror(status));
        bladerf_close(dev);
        return 2;
    }

    struct reader_state reader = {.dev = dev};
    atomic_init(&reader.running, true);
    atomic_init(&reader.blocks, 0);
    atomic_init(&reader.would_block, 0);
    atomic_init(&reader.timeouts, 0);
    atomic_init(&reader.invalid_blocks, 0);
    atomic_init(&reader.overruns, 0);
    atomic_init(&reader.bad_timestamps, 0);
    pthread_t thread;
    if (pthread_create(&thread, NULL, reader_thread, &reader) != 0) {
        fprintf(stderr, "pthread_create failed\n");
        bladerf_close(dev);
        return 2;
    }

    uint64_t event_cursor = 0;
    uint32_t event_overruns = 0;
    unsigned completed = 0;
    struct bladerf_rf_event events[BLADERF_RF_EVENT_HISTORY_SIZE];
    uint32_t count = 0;
    bool complete = false;
    status = bladerf_rf_events_get_since(
        dev, 0, events, BLADERF_RF_EVENT_HISTORY_SIZE,
        &count, &event_cursor, &complete);
    if (status != 0) goto done;

    for (unsigned i = 0; i < transitions; ++i) {
        const struct bladerf_rx_transition_request request = {
            .target_frequency_hz = (i & 1) ? 1835000000ULL : 1835400000ULL,
            .required_events_mask = BLADERF_RF_REQUIRE_PLL_LOCKED |
                                   BLADERF_RF_REQUIRE_ENSM_RX |
                                   BLADERF_RF_REQUIRE_EPOCH_VALID,
            .timeout_ms = 2000,
            .require_rx_data_valid = true,
            .epoch_settle_samples = 0,
        };
        uint32_t transaction_id = 0;
        struct bladerf_rf_event result = {0};
        if ((i % 10) == 0) {
            fprintf(stderr, "progress=%u blocks=%u would_block=%u\n", i,
                    atomic_load(&reader.blocks),
                    atomic_load(&reader.would_block));
        }
        status = bladerf_rx_transition_begin(
            dev, BLADERF_CHANNEL_RX(0), &request, &transaction_id);
        fprintf(stderr, "begin_done=%u status=%s txn=%u\n", i,
                bladerf_strerror(status), transaction_id);
        if (status != 0) {
            fprintf(stderr, "transition_begin=%u status=%s\n", i,
                    bladerf_strerror(status));
        }
        if (status == 0) status = bladerf_rx_transition_wait(
            dev, transaction_id, &result, 2000);
        fprintf(stderr, "wait_done=%u status=%s event=%u blocks=%u\n", i,
                bladerf_strerror(status), result.event_type,
                atomic_load(&reader.blocks));
        if (status != 0) {
            fprintf(stderr, "transition=%u status=%s\n", i,
                    bladerf_strerror(status));
            break;
        }
        if (result.event_type != BLADERF_RF_EVT_RX_EPOCH_VALID) {
            fprintf(stderr, "transition=%u wrong terminal event=%u\n", i,
                    result.event_type);
            status = BLADERF_ERR_UNEXPECTED;
            break;
        }
        status = collect_overruns(dev, &event_cursor, &event_overruns);
        if (status != 0) break;
        if (dwell_ms != 0) {
            /* This interval is RF observation time at the current channel,
             * not a validity delay; validity was established by FPGA events. */
            usleep((useconds_t)dwell_ms * 1000u);
        }
        completed = i + 1;
    }

done:
    /* Let the reader observe steady-state data after the final epoch. This
     * is only test duration; transition validity still comes from events. */
    usleep(250000);
    atomic_store(&reader.running, false);
    pthread_join(thread, NULL);
    printf("RX_X2 concurrent transitions=%u/%u sample_rate=%u buffer_samples=%u dwell_ms=%u blocks=%u would_block=%u "
           "timeouts=%u invalid_blocks=%u sync_overruns=%u "
           "event_overruns=%u bad_timestamps=%u\n",
           completed, transitions, sample_rate, buffer_samples, dwell_ms,
           atomic_load(&reader.blocks),
           atomic_load(&reader.would_block), atomic_load(&reader.timeouts),
           atomic_load(&reader.invalid_blocks), atomic_load(&reader.overruns),
           event_overruns, atomic_load(&reader.bad_timestamps));
    bladerf_enable_module(dev, BLADERF_CHANNEL_RX(0), false);
    bladerf_enable_module(dev, BLADERF_CHANNEL_RX(1), false);
    bladerf_close(dev);
    if (status != 0 || completed != transitions || event_overruns != 0 ||
        atomic_load(&reader.overruns) != 0 ||
        atomic_load(&reader.invalid_blocks) != 0 ||
        atomic_load(&reader.bad_timestamps) != 0) {
        return 1;
    }
    return 0;
}
