#include <libbladeRF.h>
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

static int validate_event_trace(struct bladerf *dev, uint32_t txn,
                                const struct bladerf_rf_event *final_event)
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
    if (st || !complete || count < 11 ||
        events[0].event_type != BLADERF_RF_EVT_CONFIG_ACCEPTED ||
        events[0].fpga_state != BLADERF_RF_STATE_CONFIG_PENDING ||
        events[count - 2].event_type != final_event->event_type ||
        events[count - 2].event_type != BLADERF_RF_EVT_RX_EPOCH_VALID ||
        events[count - 1].event_type !=
            BLADERF_RF_EVT_RX_FIRST_VALID_HOST_DATA ||
        events[count - 1].fpga_timestamp < final_event->fpga_timestamp) {
        fprintf(stderr, "TRACE_TERMINAL txn=%u status=%s complete=%u count=%u\n",
                txn, bladerf_strerror(st), complete, count);
        return st ? st : BLADERF_ERR_UNEXPECTED;
    }

    const bladerf_rf_event_type required[] = {
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
    unsigned next = 0;
    uint64_t spi_begin_ns = 0;
    uint64_t spi_done_ns = 0;
    uint64_t lo_return_ns = 0;
    uint32_t spi_write_count = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (events[i].transaction_id != txn ||
            (i && events[i].host_monotonic_ns < events[i - 1].host_monotonic_ns)) {
            fprintf(stderr, "TRACE_ID_OR_ORDER txn=%u index=%u\n", txn, i);
            return BLADERF_ERR_UNEXPECTED;
        }
        if (events[i].event_type == BLADERF_RF_EVT_SPI_WRITE_BEGIN) {
            if (events[i].flags == 0) return BLADERF_ERR_UNEXPECTED;
            spi_begin_ns = events[i].host_monotonic_ns;
            spi_write_count = events[i].flags;
        } else if (events[i].event_type == BLADERF_RF_EVT_SPI_DONE) {
            if (events[i].flags == 0 || spi_begin_ns == 0 ||
                events[i].host_monotonic_ns < spi_begin_ns) {
                return BLADERF_ERR_UNEXPECTED;
            }
            spi_done_ns = events[i].host_monotonic_ns;
        } else if (events[i].event_type == BLADERF_RF_EVT_LO_SET_RETURNED) {
            lo_return_ns = events[i].host_monotonic_ns;
        }
        if (next < sizeof(required) / sizeof(required[0]) &&
            events[i].event_type == required[next]) {
            if (next == 1 && events[i].fpga_state !=
                                BLADERF_RF_STATE_SPI_PROGRAMMING) {
                return BLADERF_ERR_UNEXPECTED;
            }
            ++next;
        }
    }
    if (next != sizeof(required) / sizeof(required[0]) ||
        spi_write_count == 0 || spi_done_ns == 0 || lo_return_ns == 0 ||
        spi_done_ns < spi_begin_ns || lo_return_ns < spi_done_ns) {
        return BLADERF_ERR_UNEXPECTED;
    }
    fprintf(stderr, "TRACE_SPI txn=%u writes=%u host_observed_write_us=%.3f "
            "post_spi_to_tune_return_us=%.3f\n", txn, spi_write_count,
            (spi_done_ns - spi_begin_ns) / 1000.0,
            (lo_return_ns - spi_done_ns) / 1000.0);
    return 0;
}

int main(int argc, char **argv) {
    unsigned n = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 10) : 1000;
    const bool cross_band = argc > 2 && strcmp(argv[2], "--cross-band") == 0;
    struct bladerf *dev = NULL;
    int16_t *samples = calloc(8192 * 2, sizeof(*samples));
    uint64_t *latencies_ns = calloc(n, sizeof(*latencies_ns));
    unsigned completed = 0;
    if (!samples || !latencies_ns) return 2;
    int st = bladerf_open(&dev, NULL);
    if (st) { fprintf(stderr, "open: %s\n", bladerf_strerror(st)); return 2; }
    bladerf_log_set_verbosity(BLADERF_LOG_LEVEL_WARNING);
#define CHECK(x) do { st=(x); if(st) { fprintf(stderr,"%s: %s\n",#x,bladerf_strerror(st)); goto fail; } } while(0)
    CHECK(bladerf_set_sample_rate(dev, BLADERF_CHANNEL_RX(0), 4000000, NULL));
    CHECK(bladerf_set_bandwidth(dev, BLADERF_CHANNEL_RX(0), 5000000, NULL));
    CHECK(bladerf_set_gain(dev, BLADERF_CHANNEL_RX(0), 30));
    CHECK(bladerf_sync_config(dev, BLADERF_RX_X1, BLADERF_FORMAT_SC16_Q11_META, 16, 8192, 8, 1000));
    CHECK(bladerf_enable_module(dev, BLADERF_CHANNEL_RX(0), true));
    uint64_t last_ts = 0;
    unsigned failures = 0;
    unsigned first_read_faults = 0;
    unsigned recovered_reads = 0;
    uint32_t first_txn = 0, last_txn = 0;
    for (unsigned i=0; i<n; ++i) {
        uint64_t start_ns = monotonic_ns();
        uint64_t freq = cross_band ?
            ((i & 1) ? 1835000000ULL : 947500000ULL) :
            ((i & 1) ? 1835000000ULL : 1835400000ULL);
        struct bladerf_rx_transition_request req = {
            .target_frequency_hz = freq,
            .required_events_mask = BLADERF_RF_REQUIRE_PLL_LOCKED | BLADERF_RF_REQUIRE_ENSM_RX | BLADERF_RF_REQUIRE_EPOCH_VALID,
            .timeout_ms = 2000,
            .require_rx_data_valid = true,
            .epoch_settle_samples = 0,
        };
        uint32_t txn = 0;
        struct bladerf_rf_event event = {0};
        struct bladerf_metadata meta = {0};
        st = bladerf_rx_transition_begin(dev, BLADERF_CHANNEL_RX(0), &req, &txn);
        if (!first_txn) first_txn = txn;
        last_txn = txn;
        if (!st) {
            int competing_retune = bladerf_set_frequency(
                dev, BLADERF_CHANNEL_RX(0), freq + 100000ULL);
            if (competing_retune != BLADERF_ERR_WOULD_BLOCK) {
                fprintf(stderr, "CONCURRENT_RETUNE txn=%u status=%s\n", txn,
                        bladerf_strerror(competing_retune));
                st = BLADERF_ERR_UNEXPECTED;
            }
        }
        if (!st) {
            int competing_bandwidth = bladerf_set_bandwidth(
                dev, BLADERF_CHANNEL_RX(0), 5000000, NULL);
            if (competing_bandwidth != BLADERF_ERR_WOULD_BLOCK) {
                fprintf(stderr, "CONCURRENT_BANDWIDTH txn=%u status=%s\n",
                        txn, bladerf_strerror(competing_bandwidth));
                st = BLADERF_ERR_UNEXPECTED;
            }
        }
        if (!st) {
            int competing_rate = bladerf_set_sample_rate(
                dev, BLADERF_CHANNEL_RX(0), 4000000, NULL);
            if (competing_rate != BLADERF_ERR_WOULD_BLOCK) {
                fprintf(stderr, "CONCURRENT_SAMPLE_RATE txn=%u status=%s\n",
                        txn, bladerf_strerror(competing_rate));
                st = BLADERF_ERR_UNEXPECTED;
            }
        }
        if (!st) st = bladerf_rx_transition_wait(dev, txn, &event, 2000);
        latencies_ns[i] = monotonic_ns() - start_ns;
        completed = i + 1;
        bool valid = false;
        if (!st) {
            for (unsigned attempt = 0; attempt < 4; ++attempt) {
                meta = (struct bladerf_metadata){0};
                meta.flags = BLADERF_META_FLAG_RX_NOW;
                st = bladerf_sync_rx(dev, samples, 8192, &meta, 2000);
                valid = !st && meta.rx_epoch_id_valid &&
                        meta.rx_epoch_id == event.epoch_id &&
                        meta.actual_count == 8192 &&
                        !(meta.status & BLADERF_META_STATUS_OVERRUN) &&
                        meta.timestamp >= event.fpga_timestamp &&
                        (!last_ts || meta.timestamp > last_ts);
                if (valid) {
                    if (attempt) ++recovered_reads;
                    last_ts = meta.timestamp;
                    break;
                }
                if (!attempt) ++first_read_faults;
                fprintf(stderr,"READ_FAULT i=%u attempt=%u status=%s epoch=%u/%u valid=%u count=%u meta_status=0x%x ts=%llu boundary=%llu\n",
                        i, attempt + 1, bladerf_strerror(st), meta.rx_epoch_id,
                        event.epoch_id, meta.rx_epoch_id_valid, meta.actual_count,
                        meta.status, (unsigned long long)meta.timestamp,
                        (unsigned long long)event.fpga_timestamp);
            }
        }
        if (!valid) {
            ++failures;
            fprintf(stderr,"UNRECOVERED i=%u status=%s\n", i, bladerf_strerror(st));
            if (failures >= 10) break;
        }
        if (valid) {
            st = validate_event_trace(dev, txn, &event);
            if (st != 0) {
                ++failures;
                fprintf(stderr, "TRACE_INVALID i=%u status=%s\n", i,
                        bladerf_strerror(st));
            }
        }
        if ((i+1)%100==0) fprintf(stderr,"progress=%u unrecovered=%u first_read_faults=%u recovered=%u last_epoch=%u\n",i+1,failures,first_read_faults,recovered_reads,event.epoch_id);
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
    qsort(latencies_ns, completed, sizeof(*latencies_ns), compare_u64);
    printf("transitions=%u unrecovered=%u first_read_faults=%u recovered=%u "
           "transition_ms_p50=%.3f_p95=%.3f_p99=%.3f_max=%.3f\n",
           completed, failures, first_read_faults, recovered_reads,
           completed ? latencies_ns[(completed - 1) * 50 / 100] / 1e6 : 0.0,
           completed ? latencies_ns[(completed - 1) * 95 / 100] / 1e6 : 0.0,
           completed ? latencies_ns[(completed - 1) * 99 / 100] / 1e6 : 0.0,
           completed ? latencies_ns[completed - 1] / 1e6 : 0.0);
    bladerf_enable_module(dev, BLADERF_CHANNEL_RX(0), false);
    bladerf_close(dev); free(samples); free(latencies_ns);
    return failures ? 1 : 0;
fail:
    bladerf_close(dev); free(samples); free(latencies_ns); return 2;
}
