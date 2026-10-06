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
        if ((i+1)%100==0) fprintf(stderr,"progress=%u unrecovered=%u first_read_faults=%u recovered=%u last_epoch=%u\n",i+1,failures,first_read_faults,recovered_reads,event.epoch_id);
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
