#include <libbladeRF.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

static uint64_t monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int compare_u64(const void *a, const void *b)
{
    uint64_t va = *(const uint64_t *)a;
    uint64_t vb = *(const uint64_t *)b;
    return (va > vb) - (va < vb);
}

static int collect_runtime_events(struct bladerf *dev, uint64_t *cursor,
                                  uint32_t *overruns, uint32_t *withheld,
                                  bool establish_baseline)
{
    struct bladerf_rf_event events[BLADERF_RF_EVENT_HISTORY_SIZE];
    uint32_t count = 0;
    uint64_t next = *cursor;
    bool complete = false;
    int st = bladerf_rf_events_get_since(dev, *cursor, events,
                                          BLADERF_RF_EVENT_HISTORY_SIZE,
                                          &count, &next, &complete);
    if (st != 0) return st;
    if (!establish_baseline && !complete) {
        fprintf(stderr, "RUNTIME_EVENT_HISTORY_GAP after=%llu next=%llu\n",
                (unsigned long long)*cursor, (unsigned long long)next);
        return BLADERF_ERR_UNEXPECTED;
    }
    if (!establish_baseline) {
        for (uint32_t i = 0; i < count; ++i) {
            if (events[i].event_type == BLADERF_RF_EVT_RX_STREAM_OVERRUN) {
                ++*overruns;
                if (*overruns <= 16) {
                    fprintf(stderr, "RUNTIME_OVERRUN n=%u epoch=%u flags=0x%x "
                            "rfic_status=0x%x error=%d timestamp=%llu\n",
                            *overruns, events[i].epoch_id, events[i].flags,
                            events[i].rfic_status, events[i].error_code,
                            (unsigned long long)events[i].fpga_timestamp);
                }
            } else if (events[i].event_type == BLADERF_RF_EVT_RX_DATA_WITHHELD) {
                ++*withheld;
            }
        }
    }
    *cursor = next;
    return 0;
}

static int validate_event_trace(struct bladerf *dev, uint32_t txn,
                                const struct bladerf_rf_event *final_event,
                                bool report_trace,
                                bladerf_channel transition_channel,
                                bool require_rfdc_cal)
{
    struct bladerf_rf_event events[BLADERF_RF_EVENT_HISTORY_SIZE];
    uint32_t count = 0;
    bool complete = false;
    int st = bladerf_rx_transition_get_events(dev, txn, NULL, 0, &count,
                                               &complete);
    if (st || count == 0 || count > BLADERF_RF_EVENT_HISTORY_SIZE) {
        fprintf(stderr, "TRACE_COUNT txn=%u status=%s count=%u\n", txn,
                bladerf_strerror(st), count);
        return st ? st : BLADERF_ERR_UNEXPECTED;
    }

    st = bladerf_rx_transition_get_events(dev, txn, events,
                                           BLADERF_RF_EVENT_HISTORY_SIZE,
                                           &count, &complete);
    bool epoch_valid_seen = false;
    bool host_data_seen = false;
    bool terminal_event_valid;
    uint64_t epoch_valid_ts = 0;
    for (uint32_t i = 0; i < count && i < BLADERF_RF_EVENT_HISTORY_SIZE; ++i) {
        if (events[i].event_type == BLADERF_RF_EVT_RX_EPOCH_VALID &&
            events[i].epoch_id == final_event->epoch_id) {
            epoch_valid_seen = true;
            epoch_valid_ts = events[i].fpga_timestamp;
        }
        if (epoch_valid_seen &&
            (events[i].event_type == BLADERF_RF_EVT_RX_FIRST_VALID_HOST_DATA ||
             events[i].event_type == BLADERF_RF_EVT_RX_DATA_RESUMED) &&
            events[i].epoch_id == final_event->epoch_id &&
            events[i].fpga_timestamp >= epoch_valid_ts) {
            host_data_seen = true;
        }
    }
    terminal_event_valid =
        (final_event->event_type == BLADERF_RF_EVT_RX_EPOCH_VALID ||
         final_event->event_type == BLADERF_RF_EVT_RX_FIRST_VALID_HOST_DATA) &&
        final_event->transaction_id == txn;
    if (st || !complete || count < 11 ||
        events[0].event_type != BLADERF_RF_EVT_CONFIG_ACCEPTED ||
        events[0].fpga_state != BLADERF_RF_STATE_CONFIG_PENDING ||
        !terminal_event_valid ||
        !epoch_valid_seen || !host_data_seen) {
        fprintf(stderr, "TRACE_TERMINAL txn=%u status=%s complete=%u count=%u\n",
                txn, bladerf_strerror(st), complete, count);
        for (uint32_t i = 0; i < count && i < BLADERF_RF_EVENT_HISTORY_SIZE; ++i) {
            fprintf(stderr, "TRACE_EVENT i=%u type=%u state=%u epoch=%u "
                    "flags=0x%x error=%d ts=%llu\n", i,
                    events[i].event_type, events[i].fpga_state,
                    events[i].epoch_id, events[i].flags, events[i].error_code,
                    (unsigned long long)events[i].fpga_timestamp);
        }
        return st ? st : BLADERF_ERR_UNEXPECTED;
    }
    const bladerf_rf_event_type required_normal[] = {
        BLADERF_RF_EVT_RX_EPOCH_INVALID,
        BLADERF_RF_EVT_CONFIG_ACCEPTED,
        BLADERF_RF_EVT_SPI_WRITE_BEGIN,
        BLADERF_RF_EVT_SPI_DONE,
        BLADERF_RF_EVT_LO_SET_RETURNED,
        BLADERF_RF_EVT_LO_READBACK_MATCH,
        BLADERF_RF_EVT_RX_PLL_LOCKED,
        BLADERF_RF_EVT_ENSM_RX,
        BLADERF_RF_EVT_RX_EPOCH_VALID,
        BLADERF_RF_EVT_RX_FIRST_VALID_HOST_DATA,
    };
    const bladerf_rf_event_type required_cal[] = {
        BLADERF_RF_EVT_RX_EPOCH_INVALID,
        BLADERF_RF_EVT_CONFIG_ACCEPTED,
        BLADERF_RF_EVT_SPI_WRITE_BEGIN,
        BLADERF_RF_EVT_SPI_DONE,
        BLADERF_RF_EVT_LO_SET_RETURNED,
        BLADERF_RF_EVT_LO_READBACK_MATCH,
        BLADERF_RF_EVT_RX_PLL_LOCKED,
        BLADERF_RF_EVT_RX_RFDC_CAL_DONE,
        BLADERF_RF_EVT_RX_PLL_LOCKED,
        BLADERF_RF_EVT_ENSM_RX,
        BLADERF_RF_EVT_RX_EPOCH_VALID,
        BLADERF_RF_EVT_RX_FIRST_VALID_HOST_DATA,
    };
    const bladerf_rf_event_type *required =
        require_rfdc_cal ? required_cal : required_normal;
    const size_t required_count = require_rfdc_cal
        ? sizeof(required_cal) / sizeof(required_cal[0])
        : sizeof(required_normal) / sizeof(required_normal[0]);
    unsigned next = 0;
    uint64_t spi_begin_ns = 0;
    uint64_t spi_done_ns = 0;
    uint64_t lo_return_ns = 0;
    uint32_t spi_write_count = 0;
    const uint32_t expected_channel_flags =
        BLADERF_RF_EVENT_F_TRANSITION_CHANNEL_VALID |
        (transition_channel == BLADERF_CHANNEL_RX(1)
            ? BLADERF_RF_EVENT_F_TRANSITION_RX2 : 0);
    for (uint32_t i = 0; i < count; ++i) {
        if (events[i].transaction_id != txn ||
            (i && events[i].host_monotonic_ns < events[i - 1].host_monotonic_ns)) {
            fprintf(stderr, "TRACE_ID_OR_ORDER txn=%u index=%u\n", txn, i);
            return BLADERF_ERR_UNEXPECTED;
        }
        if (events[i].event_type == BLADERF_RF_EVT_SPI_WRITE_BEGIN) {
            if ((events[i].flags & (BLADERF_RF_EVENT_F_TRANSITION_CHANNEL_VALID |
                                    BLADERF_RF_EVENT_F_TRANSITION_RX2)) !=
                    expected_channel_flags ||
                (events[i].flags & 0x0fffffffU) == 0) {
                return BLADERF_ERR_UNEXPECTED;
            }
            spi_begin_ns = events[i].host_monotonic_ns;
            spi_write_count = events[i].flags & 0x0fffffffU;
        } else if (events[i].event_type == BLADERF_RF_EVT_SPI_DONE) {
            if ((events[i].flags & 0x0fffffffU) != spi_write_count ||
                (events[i].flags & (BLADERF_RF_EVENT_F_TRANSITION_CHANNEL_VALID |
                                    BLADERF_RF_EVENT_F_TRANSITION_RX2)) !=
                    expected_channel_flags || spi_begin_ns == 0 ||
                events[i].host_monotonic_ns < spi_begin_ns) {
                return BLADERF_ERR_UNEXPECTED;
            }
            spi_done_ns = events[i].host_monotonic_ns;
        } else if (events[i].event_type == BLADERF_RF_EVT_LO_SET_RETURNED) {
            lo_return_ns = events[i].host_monotonic_ns;
        }
        if (next < required_count &&
            events[i].event_type == required[next]) {
            if (next == 1 && events[i].fpga_state !=
                                BLADERF_RF_STATE_SPI_PROGRAMMING) {
                return BLADERF_ERR_UNEXPECTED;
            }
            ++next;
        }
    }
    if (next != required_count ||
        spi_write_count == 0 || spi_done_ns == 0 || lo_return_ns == 0 ||
        spi_done_ns < spi_begin_ns || lo_return_ns < spi_done_ns) {
        return BLADERF_ERR_UNEXPECTED;
    }
    if (report_trace) {
        fprintf(stderr, "TRACE_SPI txn=%u writes=%u host_observed_write_us=%.3f "
                "post_spi_to_tune_return_us=%.3f\n", txn, spi_write_count,
                (spi_done_ns - spi_begin_ns) / 1000.0,
                (lo_return_ns - spi_done_ns) / 1000.0);
    }
    return 0;
}

int main(int argc, char **argv) {
    unsigned n = 1000;
    unsigned stream_buffer_samples = 8192;
    unsigned stream_num_buffers = 16;
    unsigned stream_num_transfers = 8;
    unsigned capture_samples = 8192;
    uint32_t sample_rate = 4000000;
    uint32_t stream_timeout_ms = 1000;
    uint32_t transition_timeout_ms = 2000;
    bool count_set = false;
    bool cross_band = false;
    bool require_rfdc_cal = false;
    bool paired = false;
    bool close_after_capture = false;
    unsigned close_pause_ms = 0;
    const char *mode = "RX1";
    bladerf_channel transition_channel = BLADERF_CHANNEL_RX(0);
    bladerf_channel_layout layout = BLADERF_RX_X1;
    for (int arg = 1; arg < argc; ++arg) {
        char *end = NULL;
        unsigned long parsed = strtoul(argv[arg], &end, 10);
        if (!count_set && end != argv[arg] && *end == '\0') {
            if (parsed == 0 || parsed > UINT32_MAX) {
                fprintf(stderr, "invalid transition count: %s\n", argv[arg]);
                return 2;
            }
            n = (unsigned)parsed;
            count_set = true;
        } else if (strcmp(argv[arg], "RX1") == 0 ||
                   strcmp(argv[arg], "RX2") == 0 ||
                   strcmp(argv[arg], "BOTH") == 0) {
            mode = argv[arg];
        } else if (strcmp(argv[arg], "--cross-band") == 0) {
            cross_band = true;
        } else if (strcmp(argv[arg], "--require-rfdc-cal") == 0) {
            require_rfdc_cal = true;
        } else if (strcmp(argv[arg], "--close-pause-ms") == 0 &&
                   arg + 1 < argc) {
            char *pause_end = NULL;
            unsigned long pause = strtoul(argv[++arg], &pause_end, 10);
            if (pause_end == argv[arg] || *pause_end != '\0' ||
                pause > 60000) {
                fprintf(stderr, "invalid --close-pause-ms value\n");
                return 2;
            }
            close_pause_ms = (unsigned)pause;
            close_after_capture = true;
        } else if (strcmp(argv[arg], "--close-after-capture") == 0) {
            close_after_capture = true;
        } else {
            fprintf(stderr, "usage: %s [count] [RX1|RX2|BOTH] [--cross-band] "
                    "[--require-rfdc-cal] [--close-after-capture] "
                    "[--close-pause-ms N]\n",
                    argv[0]);
            return 2;
        }
    }
    if (strcmp(mode, "RX2") == 0) {
        transition_channel = BLADERF_CHANNEL_RX(1);
    } else if (strcmp(mode, "BOTH") == 0) {
        layout = BLADERF_RX_X2;
        paired = true;
    }
    const char *buffer_env = getenv("BLADERF_QUAL_STREAM_BUFFER_SAMPLES");
    if (buffer_env != NULL && buffer_env[0] != '\0') {
        char *end = NULL;
        unsigned long parsed = strtoul(buffer_env, &end, 10);
        if (end == buffer_env || *end != '\0' ||
            (parsed != 8192 && parsed != 32768 &&
             parsed != 65536 && parsed != 131072)) {
            fprintf(stderr, "BLADERF_QUAL_STREAM_BUFFER_SAMPLES must be "
                    "8192, 32768, 65536, or 131072\n");
            return 2;
        }
        stream_buffer_samples = (unsigned)parsed;
        if (parsed == 32768) {
            stream_num_buffers = 64;
            stream_num_transfers = 32;
        } else if (parsed == 131072) {
            stream_num_buffers = 512;
            stream_num_transfers = 24;
        }
    }
    const char *capture_env = getenv("BLADERF_QUAL_CAPTURE_SAMPLES");
    if (capture_env != NULL && capture_env[0] != '\0') {
        char *end = NULL;
        unsigned long parsed = strtoul(capture_env, &end, 10);
        if (end == capture_env || *end != '\0' || parsed == 0 ||
            parsed > UINT32_MAX / 2) {
            fprintf(stderr, "invalid BLADERF_QUAL_CAPTURE_SAMPLES\n");
            return 2;
        }
        capture_samples = (unsigned)parsed;
    }
    const char *rate_env = getenv("BLADERF_QUAL_SAMPLE_RATE");
    if (rate_env != NULL && rate_env[0] != '\0') {
        char *end = NULL;
        unsigned long parsed = strtoul(rate_env, &end, 10);
        if (end == rate_env || *end != '\0' || parsed < 1000000 ||
            parsed > 61440000) {
            fprintf(stderr, "invalid BLADERF_QUAL_SAMPLE_RATE\n");
            return 2;
        }
        sample_rate = (uint32_t)parsed;
    }
    const char *stream_timeout_env =
        getenv("BLADERF_QUAL_STREAM_TIMEOUT_MS");
    if (stream_timeout_env != NULL && stream_timeout_env[0] != '\0') {
        char *end = NULL;
        unsigned long parsed = strtoul(stream_timeout_env, &end, 10);
        if (end == stream_timeout_env || *end != '\0' || parsed < 100 ||
            parsed > 120000) {
            fprintf(stderr, "invalid BLADERF_QUAL_STREAM_TIMEOUT_MS\n");
            return 2;
        }
        stream_timeout_ms = (uint32_t)parsed;
    }
    const char *transition_timeout_env =
        getenv("BLADERF_QUAL_TRANSITION_TIMEOUT_MS");
    if (transition_timeout_env != NULL && transition_timeout_env[0] != '\0') {
        char *end = NULL;
        unsigned long parsed = strtoul(transition_timeout_env, &end, 10);
        if (end == transition_timeout_env || *end != '\0' || parsed < 100 ||
            parsed > 120000) {
            fprintf(stderr, "invalid BLADERF_QUAL_TRANSITION_TIMEOUT_MS\n");
            return 2;
        }
        transition_timeout_ms = (uint32_t)parsed;
    }
    const char *num_buffers_env = getenv("BLADERF_QUAL_STREAM_NUM_BUFFERS");
    if (num_buffers_env != NULL && num_buffers_env[0] != '\0') {
        char *end = NULL;
        unsigned long parsed = strtoul(num_buffers_env, &end, 10);
        if (end == num_buffers_env || *end != '\0' ||
            parsed < stream_num_transfers || parsed > 1024) {
            fprintf(stderr, "BLADERF_QUAL_STREAM_NUM_BUFFERS must be "
                    "between num_transfers and 1024\n");
            return 2;
        }
        stream_num_buffers = (unsigned)parsed;
    }
    const char *num_transfers_env = getenv("BLADERF_QUAL_STREAM_NUM_TRANSFERS");
    if (num_transfers_env != NULL && num_transfers_env[0] != '\0') {
        char *end = NULL;
        unsigned long parsed = strtoul(num_transfers_env, &end, 10);
        if (end == num_transfers_env || *end != '\0' || parsed == 0 ||
            parsed > stream_num_buffers) {
            fprintf(stderr, "BLADERF_QUAL_STREAM_NUM_TRANSFERS must be "
                    "between 1 and num_buffers\n");
            return 2;
        }
        stream_num_transfers = (unsigned)parsed;
    }
    struct bladerf *dev = NULL;
    size_t sample_words = (size_t)capture_samples * (paired ? 4 : 2);
    int16_t *samples = calloc(sample_words, sizeof(*samples));
    uint64_t *latencies_ns = calloc(n, sizeof(*latencies_ns));
    unsigned completed = 0;
    if (!samples || !latencies_ns) return 2;
    int st = bladerf_open(&dev, NULL);
    if (st) { fprintf(stderr, "open: %s\n", bladerf_strerror(st)); return 2; }
    bladerf_log_set_verbosity(getenv("BLADERF_QUAL_DEBUG") != NULL
        ? BLADERF_LOG_LEVEL_DEBUG : BLADERF_LOG_LEVEL_WARNING);
#define CHECK(x) do { st=(x); if(st) { fprintf(stderr,"%s: %s\n",#x,bladerf_strerror(st)); goto fail; } } while(0)
    CHECK(bladerf_set_sample_rate(dev, transition_channel, sample_rate, NULL));
    CHECK(bladerf_set_bandwidth(dev, transition_channel,
                                sample_rate > 20000000 ? 20000000 : 5000000,
                                NULL));
    if (paired) {
        CHECK(bladerf_set_sample_rate(dev, BLADERF_CHANNEL_RX(1), sample_rate, NULL));
        CHECK(bladerf_set_bandwidth(dev, BLADERF_CHANNEL_RX(1),
                                    sample_rate > 20000000 ? 20000000 : 5000000,
                                    NULL));
    }
    CHECK(bladerf_set_gain(dev, transition_channel, 30));
    CHECK(bladerf_sync_config(dev, layout, BLADERF_FORMAT_SC16_Q11_META,
                              stream_num_buffers, stream_buffer_samples,
                              stream_num_transfers, stream_timeout_ms));
    CHECK(bladerf_enable_module(dev, transition_channel, true));
    if (paired) CHECK(bladerf_enable_module(dev, BLADERF_CHANNEL_RX(1), true));
    uint64_t runtime_event_cursor = 0;
    uint32_t stream_overrun_events = 0;
    uint32_t data_withheld_events = 0;
    CHECK(collect_runtime_events(dev, &runtime_event_cursor,
                                 &stream_overrun_events, &data_withheld_events,
                                 true));
    uint64_t last_ts = 0;
    unsigned failures = 0;
    unsigned first_read_faults = 0;
    unsigned recovered_reads = 0;
    unsigned retry_would_block = 0;
    unsigned retry_overruns = 0;
    uint32_t first_txn = 0, last_txn = 0;
    for (unsigned i=0; i<n; ++i) {
        uint64_t start_ns = monotonic_ns();
        uint64_t freq = cross_band ?
            ((i & 1) ? 1835000000ULL : 947500000ULL) :
            ((i & 1) ? 1835000000ULL : 1835400000ULL);
        struct bladerf_rx_transition_request req = {
            .target_frequency_hz = freq,
            .required_events_mask = BLADERF_RF_REQUIRE_PLL_LOCKED |
                BLADERF_RF_REQUIRE_ENSM_RX |
                BLADERF_RF_REQUIRE_EPOCH_VALID |
                (require_rfdc_cal
                    ? BLADERF_RF_REQUIRE_RX_RFDC_CAL_DONE : 0) |
                (paired ? BLADERF_RF_REQUIRE_RX_X2_HOST_DATA : 0),
            .timeout_ms = transition_timeout_ms,
            .require_rx_data_valid = true,
            .epoch_settle_samples = 0,
        };
        uint32_t txn = 0;
        struct bladerf_rf_event event = {0};
        struct bladerf_metadata meta = {0};
        st = bladerf_rx_transition_begin(dev, transition_channel, &req, &txn);
        if (!first_txn) first_txn = txn;
        last_txn = txn;
        if (!st) {
            int competing_retune = bladerf_set_frequency(
                dev, transition_channel, freq + 100000ULL);
            if (competing_retune != BLADERF_ERR_WOULD_BLOCK) {
                fprintf(stderr, "CONCURRENT_RETUNE txn=%u status=%s\n", txn,
                        bladerf_strerror(competing_retune));
                st = BLADERF_ERR_UNEXPECTED;
            }
        }
        if (!st) {
            int competing_bandwidth = bladerf_set_bandwidth(
                dev, transition_channel, 5000000, NULL);
            if (competing_bandwidth != BLADERF_ERR_WOULD_BLOCK) {
                fprintf(stderr, "CONCURRENT_BANDWIDTH txn=%u status=%s\n",
                        txn, bladerf_strerror(competing_bandwidth));
                st = BLADERF_ERR_UNEXPECTED;
            }
        }
        if (!st) {
            int competing_rate = bladerf_set_sample_rate(
                dev, transition_channel, sample_rate, NULL);
            if (competing_rate != BLADERF_ERR_WOULD_BLOCK) {
                fprintf(stderr, "CONCURRENT_SAMPLE_RATE txn=%u status=%s\n",
                        txn, bladerf_strerror(competing_rate));
                st = BLADERF_ERR_UNEXPECTED;
            }
        }
        if (!st) {
            st = bladerf_rx_transition_wait(dev, txn, &event, 2000);
            if (st != 0) {
                fprintf(stderr, "TRANSITION_WAIT_FAILED i=%u txn=%u status=%s\n",
                        i, txn, bladerf_strerror(st));
            }
        }
        latencies_ns[i] = monotonic_ns() - start_ns;
        completed = i + 1;
        bool valid = false;
        uint64_t capture_started_ns = monotonic_ns();
        if (!st) {
            for (unsigned attempt = 0; attempt < 4; ++attempt) {
                meta = (struct bladerf_metadata){0};
                meta.flags = BLADERF_META_FLAG_RX_NOW;
                st = bladerf_sync_rx(dev, samples,
                                     capture_samples * (paired ? 2 : 1),
                                     &meta, 5000);
                valid = !st && meta.rx_epoch_id_valid &&
                        meta.rx_epoch_id == event.epoch_id &&
                        meta.actual_count == capture_samples * (paired ? 2 : 1) &&
                        !(meta.status & BLADERF_META_STATUS_OVERRUN) &&
                        meta.timestamp >= event.fpga_timestamp &&
                        (!last_ts || meta.timestamp > last_ts);
                if (valid) {
                    if (attempt) ++recovered_reads;
                    last_ts = meta.timestamp;
                    break;
                }
                if (st == BLADERF_ERR_WOULD_BLOCK) ++retry_would_block;
                if (meta.status & BLADERF_META_STATUS_OVERRUN) ++retry_overruns;
                if (!attempt) ++first_read_faults;
                if (attempt == 3) {
                    fprintf(stderr,"READ_FAULT i=%u attempt=%u status=%s epoch=%u/%u valid=%u count=%u meta_status=0x%x ts=%llu boundary=%llu\n",
                            i, attempt + 1, bladerf_strerror(st), meta.rx_epoch_id,
                            event.epoch_id, meta.rx_epoch_id_valid, meta.actual_count,
                            meta.status, (unsigned long long)meta.timestamp,
                            (unsigned long long)event.fpga_timestamp);
                }
            }
        }
        if (!valid) {
            ++failures;
            fprintf(stderr,"UNRECOVERED i=%u status=%s\n", i, bladerf_strerror(st));
            if (failures >= 10) break;
        }
        if (getenv("BLADERF_QUAL_CAPTURE_SAMPLES") != NULL && i < 16) {
            fprintf(stderr, "CAPTURE_TIMING i=%u samples_per_channel=%u "
                    "sample_rate=%u duration_ms=%.3f status=%s\n", i,
                    capture_samples, sample_rate,
                    (monotonic_ns() - capture_started_ns) / 1e6,
                    bladerf_strerror(st));
        }
        if (valid) {
            bool capture_closed = false;
            if (getenv("BLADERF_QUAL_CLOSE_BEFORE_TRACE") != NULL) {
                st = bladerf_rx_capture_close(dev, transition_channel);
                if (st != 0) {
                    ++failures;
                    fprintf(stderr, "CAPTURE_CLOSE_FAILED i=%u status=%s\n",
                            i, bladerf_strerror(st));
                    valid = false;
                } else {
                    capture_closed = true;
                    fprintf(stderr, "CAPTURE_CLOSED_BEFORE_TRACE i=%u\n", i);
                }
            }
            if (valid) {
            st = validate_event_trace(dev, txn, &event,
                                      (i + 1) % 100 == 0,
                                      transition_channel,
                                      require_rfdc_cal);
            if (st != 0) {
                ++failures;
                fprintf(stderr, "TRACE_INVALID i=%u status=%s\n", i,
                        bladerf_strerror(st));
            }
            }
            if (capture_closed) {
                fprintf(stderr, "CAPTURE_CLOSED i=%u epoch=%u pause_ms=%u\n",
                        i, event.epoch_id, close_pause_ms);
                if (close_pause_ms != 0) {
                    struct timespec pause = {
                        .tv_sec = close_pause_ms / 1000,
                        .tv_nsec = (long)(close_pause_ms % 1000) * 1000000L,
                    };
                    while (nanosleep(&pause, &pause) != 0 && errno == EINTR) {}
                }
            }
        }
        if (valid && close_after_capture &&
            getenv("BLADERF_QUAL_CLOSE_BEFORE_TRACE") == NULL) {
            st = bladerf_rx_capture_close(dev, transition_channel);
            if (st != 0) {
                ++failures;
                fprintf(stderr, "CAPTURE_CLOSE_FAILED i=%u status=%s\n",
                        i, bladerf_strerror(st));
            } else {
                fprintf(stderr, "CAPTURE_CLOSED i=%u epoch=%u pause_ms=%u\n",
                        i, event.epoch_id, close_pause_ms);
                struct timespec pause = {
                    .tv_sec = close_pause_ms / 1000,
                    .tv_nsec = (long)(close_pause_ms % 1000) * 1000000L,
                };
                while (nanosleep(&pause, &pause) != 0) {
                    if (errno != EINTR) {
                        perror("nanosleep");
                        st = BLADERF_ERR_UNEXPECTED;
                        break;
                    }
                }
                if (st != 0) {
                    ++failures;
                    break;
                }
            }
        }
        CHECK(collect_runtime_events(dev, &runtime_event_cursor,
                                     &stream_overrun_events,
                                     &data_withheld_events, false));
        if ((i+1)%100==0) fprintf(stderr,"mode=%s progress=%u unrecovered=%u first_read_faults=%u recovered=%u last_epoch=%u\n",mode,i+1,failures,first_read_faults,recovered_reads,event.epoch_id);
    }
    if (!failures && completed > BLADERF_RF_EVENT_HISTORY_SIZE / 6) {
        uint32_t retained = 0;
        bool complete = true;
        CHECK(bladerf_rx_transition_get_events(dev, first_txn, NULL, 0,
                                                &retained, &complete));
        if (retained != 0 || complete) {
            fprintf(stderr, "TRACE_OVERWRITE txn=%u retained=%u complete=%u\n",
                    first_txn, retained, complete);
            st = BLADERF_ERR_UNEXPECTED;
            goto fail;
        }
        CHECK(bladerf_rx_transition_get_events(dev, last_txn, NULL, 0,
                                                &retained, &complete));
        if (retained == 0 || !complete) {
            fprintf(stderr, "TRACE_LATEST txn=%u retained=%u complete=%u\n",
                    last_txn, retained, complete);
            st = BLADERF_ERR_UNEXPECTED;
            goto fail;
        }
    }
    fprintf(stderr, "FINAL_EVENT_HISTORY_OK first=%u last=%u\n",
            first_txn, last_txn);
    qsort(latencies_ns, completed, sizeof(*latencies_ns), compare_u64);
    printf("mode=%s transitions=%u unrecovered=%u first_read_faults=%u recovered=%u "
           "retry_would_block=%u retry_overruns=%u stream_overrun_events=%u "
           "data_withheld_events=%u "
           "transition_ms_p50=%.3f_p95=%.3f_p99=%.3f_max=%.3f\n",
           mode, completed, failures, first_read_faults, recovered_reads,
           retry_would_block, retry_overruns, stream_overrun_events,
           data_withheld_events,
           completed ? latencies_ns[(completed - 1) * 50 / 100] / 1e6 : 0.0,
           completed ? latencies_ns[(completed - 1) * 95 / 100] / 1e6 : 0.0,
           completed ? latencies_ns[(completed - 1) * 99 / 100] / 1e6 : 0.0,
           completed ? latencies_ns[completed - 1] / 1e6 : 0.0);
    if (stream_overrun_events != 0) {
        fprintf(stderr, "QUALIFICATION_FAILED runtime_stream_overruns=%u\n",
                stream_overrun_events);
    }
    bladerf_enable_module(dev, transition_channel, false);
    fprintf(stderr, "FINAL_RX1_DISABLE_DONE mode=%s\n", mode);
    if (paired) bladerf_enable_module(dev, BLADERF_CHANNEL_RX(1), false);
    fprintf(stderr, "FINAL_RX2_DISABLE_DONE paired=%u\n", paired);
    fprintf(stderr, "FINAL_DEVICE_CLOSE_BEGIN\n");
    bladerf_close(dev);
    fprintf(stderr, "FINAL_DEVICE_CLOSE_DONE\n");
    free(samples); free(latencies_ns);
    return (failures || stream_overrun_events) ? 1 : 0;
fail:
    bladerf_close(dev); free(samples); free(latencies_ns); return 2;
}
