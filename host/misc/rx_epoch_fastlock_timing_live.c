/* Fastlock (NIOS RETUNE2) vs host-tuned RX transition timing.
 * Mode "ab": paired A/B. Trials 2k and 2k+1 form a pair; which of the two
 * runs fastlock is drawn from a seeded PRNG (seed printed; replay with
 * --seed=N), so arm assignment is independent of trial parity and of the
 * transition direction. Usage: [trials] [fastlock|host|ab] [--cross-band]
 * [--seed=N].
 * PASS: every trial valid (complete trace, ordered events, no NIOS transport
 * error), no USB timeout; p50/p99/max total_to_epoch_us printed per arm. No
 * latency threshold is defined; the numbers are the result. */
#include <libbladeRF.h>
#include "hwqual_live.h"
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RX_CHANNEL BLADERF_CHANNEL_RX(0)
#define RX_SAMPLE_RATE 4000000
#define RX_BANDWIDTH 5000000
#define LOCAL_FREQ_A 1835000000ULL
#define LOCAL_FREQ_B 1835400000ULL
#define CROSS_FREQ_A 947500000ULL
#define CROSS_FREQ_B 1835000000ULL
/* Diagnostics only, not a PASS limit: a transition slower than this prints
 * its full event trace (event types incl. RX_RFDC_CAL_DONE, rfic_status), so
 * a single slow transition in a long run names its stage. */
#define TRACE_SLOW_US 100000.0

static int fail_status(const char *what, int status)
{
    fprintf(stderr, "%s: %s (%d)\n", what, bladerf_strerror(status), status);
    return 1;
}

static int event_time(const struct bladerf_rf_event *events, uint32_t count,
                      bladerf_rf_event_type type, uint64_t *timestamp)
{
    for (uint32_t i = 0; i < count; ++i) {
        if (events[i].event_type == type) {
            *timestamp = events[i].host_monotonic_ns;
            return 0;
        }
    }
    return -1;
}

int main(int argc, char **argv)
{
    const unsigned trials = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 10) : 20;
    const char *mode = argc > 2 ? argv[2] : "fastlock";
    bool cross_band = false;
    /* HQ_TRACE_SLOW_MS lowers the trace threshold for a diagnostic run. */
    const char *trace_env = getenv("HQ_TRACE_SLOW_MS");
    const double trace_slow_us =
        trace_env != NULL ? strtod(trace_env, NULL) * 1000.0 : TRACE_SLOW_US;
    uint64_t seed = 0;
    for (int a = 3; a < argc; ++a) {
        if (strcmp(argv[a], "--cross-band") == 0) cross_band = true;
        else if (strncmp(argv[a], "--seed=", 7) == 0)
            seed = strtoull(argv[a] + 7, NULL, 0);
        else if (argv[a][0] != 0) {
            fprintf(stderr, "unknown argument: %s\n", argv[a]);
            return 2;
        }
    }
    seed = hq_seed_or_time(seed);
    uint64_t rng = seed;
    if (getenv("BLADERF_RX_EPOCH_POLL_TRACE") != NULL) {
        bladerf_log_set_verbosity(BLADERF_LOG_LEVEL_DEBUG);
    }
    const uint64_t frequencies[2] = {
        cross_band ? CROSS_FREQ_A : LOCAL_FREQ_A,
        cross_band ? CROSS_FREQ_B : LOCAL_FREQ_B,
    };
    struct bladerf *dev = NULL;
    struct bladerf_quick_tune profiles[2];
    bool *arm_fastlock = NULL;
    double *total_us[2] = {NULL, NULL};
    size_t arm_n[2] = {0, 0};
    unsigned arm_dir[2][2] = {{0, 0}, {0, 0}};
    unsigned rfdc_transitions = 0, first_rfdc_trial = 0;
    uint32_t first_transaction_id = 0;
    uint32_t last_transaction_id = 0;
    bool last_transaction_fastlock = false;
    int status;
    int result = 1;

    if (trials == 0 || (strcmp(mode, "fastlock") != 0 &&
                        strcmp(mode, "host") != 0 &&
                        strcmp(mode, "ab") != 0)) {
        fputs("usage: rx_epoch_fastlock_timing_live [trials] [fastlock|host|ab] "
              "[--cross-band]\n",
              stderr);
        return 2;
    }
    status = bladerf_open(&dev, NULL);
    if (status != 0) return fail_status("open", status);

    if ((status = bladerf_set_sample_rate(dev, RX_CHANNEL, RX_SAMPLE_RATE, NULL)) != 0 ||
        (status = bladerf_set_bandwidth(dev, RX_CHANNEL, RX_BANDWIDTH, NULL)) != 0 ||
        (status = bladerf_set_frequency(dev, RX_CHANNEL, frequencies[0])) != 0 ||
        (status = bladerf_get_quick_tune(dev, RX_CHANNEL, &profiles[0])) != 0 ||
        (status = bladerf_set_frequency(dev, RX_CHANNEL, frequencies[1])) != 0 ||
        (status = bladerf_get_quick_tune(dev, RX_CHANNEL, &profiles[1])) != 0 ||
        (status = bladerf_set_frequency(dev, RX_CHANNEL, frequencies[0])) != 0 ||
        (status = bladerf_sync_config(dev, BLADERF_RX_X1,
                                      BLADERF_FORMAT_SC16_Q11_META,
                                      16, 8192, 8, 0)) != 0 ||
        (status = bladerf_enable_module(dev, RX_CHANNEL, true)) != 0) {
        fail_status("configure fastlock timing test", status);
        goto done;
    }

    arm_fastlock = calloc(trials, sizeof(*arm_fastlock));
    total_us[0] = calloc(trials, sizeof(double));
    total_us[1] = calloc(trials, sizeof(double));
    if (arm_fastlock == NULL || total_us[0] == NULL || total_us[1] == NULL) {
        fputs("allocation failed\n", stderr);
        goto done;
    }
    for (unsigned p = 0; p < trials; p += 2) {
        const bool first = strcmp(mode, "fastlock") == 0 ||
            (strcmp(mode, "ab") == 0 && (hq_rng_next(&rng) >> 63));
        arm_fastlock[p] = first;
        if (p + 1 < trials)
            arm_fastlock[p + 1] = strcmp(mode, "ab") == 0 ? !first : first;
    }
    printf("seed=%" PRIu64 "\n", seed);
    printf("transition timing mode=%s trials=%u profiles=%u/%u frequencies=%llu/%llu\n",
           mode, trials, profiles[0].rffe_profile, profiles[1].rffe_profile,
           (unsigned long long)frequencies[0],
           (unsigned long long)frequencies[1]);

    for (unsigned i = 0; i < trials; ++i) {
        const unsigned target = (i + 1) & 1u;
        const bool use_fastlock = arm_fastlock[i];
        const struct bladerf_rx_transition_request request = {
            .target_frequency_hz = frequencies[target],
            .required_events_mask = BLADERF_RF_REQUIRE_PLL_LOCKED |
                                    BLADERF_RF_REQUIRE_ENSM_RX |
                                    BLADERF_RF_REQUIRE_EPOCH_VALID,
            .timeout_ms = 0,
            .require_rx_data_valid = true,
            .epoch_settle_samples = 0,
        };
        struct bladerf_rf_event events[BLADERF_RF_EVENT_HISTORY_SIZE];
        struct bladerf_rf_event final_event = {0};
        struct bladerf_rx_transition_nios_timing nios_timing = {0};
        uint32_t transaction = 0, count = 0;
        bool complete = false;
        uint64_t begin_ns = 0, out_ns = 0, response_ns = 0;
        uint64_t spi_begin_ns = 0, spi_done_ns = 0, lo_return_ns = 0;
        uint64_t config_ns = 0;

        status = use_fastlock ?
            bladerf_rx_transition_begin_quick_tune(
                dev, RX_CHANNEL, &request, &profiles[target], &transaction) :
            bladerf_rx_transition_begin(
                dev, RX_CHANNEL, &request, &transaction);
        if (status == 0) {
            status = bladerf_rx_transition_wait(dev, transaction, &final_event,
                                                request.timeout_ms);
        }
        if (first_transaction_id == 0) {
            first_transaction_id = transaction;
        }
        last_transaction_id = transaction;
        last_transaction_fastlock = use_fastlock;
        if (status == 0) {
            status = bladerf_rx_transition_get_events(
                dev, transaction, events, BLADERF_RF_EVENT_HISTORY_SIZE,
                &count, &complete);
        }
        if (status == 0) {
            status = bladerf_rx_transition_get_nios_timing(
                dev, transaction, &nios_timing);
        }
        const bool nios_events =
            event_time(events, count, BLADERF_RF_EVT_NIOS_RETUNE_BEGIN,
                       &begin_ns) == 0 &&
            event_time(events, count, BLADERF_RF_EVT_NIOS_RETUNE_USB_OUT_DONE,
                       &out_ns) == 0 &&
            event_time(events, count, BLADERF_RF_EVT_NIOS_RETUNE_RESPONSE,
                       &response_ns) == 0;
        const bool host_events =
            event_time(events, count, BLADERF_RF_EVT_SPI_WRITE_BEGIN,
                       &spi_begin_ns) == 0 &&
            event_time(events, count, BLADERF_RF_EVT_SPI_DONE,
                       &spi_done_ns) == 0 &&
            event_time(events, count, BLADERF_RF_EVT_LO_SET_RETURNED,
                       &lo_return_ns) == 0;
        const bool have_config =
            event_time(events, count, BLADERF_RF_EVT_CONFIG_ACCEPTED,
                       &config_ns) == 0;
        if (status != 0 || !complete || !have_config ||
            !nios_timing.transaction_retained ||
            nios_timing.nios_retune_observed != use_fastlock ||
            (use_fastlock && (!nios_events || begin_ns > out_ns ||
                              out_ns > response_ns || host_events)) ||
            (!use_fastlock && (!host_events || nios_events ||
                               spi_begin_ns < config_ns ||
                               spi_done_ns < spi_begin_ns ||
                               lo_return_ns < spi_done_ns)) ||
            final_event.event_type != BLADERF_RF_EVT_RX_EPOCH_VALID) {
            fprintf(stderr, "invalid fastlock trace trial=%u txn=%u status=%s "
                    "complete=%u events=%u final=%u\n", i, transaction,
                    bladerf_strerror(status), complete, count,
                    final_event.event_type);
            goto done;
        }

        for (uint32_t j = 0; j < count; ++j) {
            if (events[j].transaction_id != transaction ||
                (j && events[j].host_monotonic_ns <
                      events[j - 1].host_monotonic_ns)) {
                fprintf(stderr, "non-monotonic/stale trace trial=%u index=%u\n",
                        i, j);
                goto done;
            }
            if (events[j].event_type == BLADERF_RF_EVT_NIOS_RETUNE_RESPONSE &&
                (events[j].error_code != 0 || events[j].rfic_status != 0)) {
                fprintf(stderr, "NIOS transport error trial=%u code=%d status=%u\n",
                        i, events[j].error_code, events[j].rfic_status);
                goto done;
            }
        }
        if (getenv("BLADERF_RX_EPOCH_POLL_TRACE") != NULL ||
            (final_event.host_monotonic_ns - config_ns) / 1000.0 > trace_slow_us) {
            for (uint32_t j = 0; j < count; ++j) {
                printf("event txn=%u index=%u type=%u state=%u "
                       "since_config_us=%.3f fpga_ts=%llu flags=0x%x "
                       "rfic_status=0x%x error=%d\n",
                       transaction, j, events[j].event_type,
                       events[j].fpga_state,
                       (events[j].host_monotonic_ns - config_ns) / 1000.0,
                       (unsigned long long)events[j].fpga_timestamp,
                       events[j].flags, events[j].rfic_status, events[j].error_code);
            }
        }
        {
            const double t = (final_event.host_monotonic_ns - config_ns) / 1000.0;
            total_us[use_fastlock][arm_n[use_fastlock]++] = t;
            ++arm_dir[use_fastlock][target];
            for (uint32_t j = 0; j < count; ++j) {
                if (events[j].event_type == BLADERF_RF_EVT_RX_RFDC_CAL_DONE) {
                    if (rfdc_transitions++ == 0) first_rfdc_trial = i;
                    break;
                }
            }
        }
        if (use_fastlock) {
            /* Tokened asynchronous NOW acceptance has no device duration. */
            printf("trial=%u mode=fastlock txn=%u epoch=%u usb_out_us=%.3f "
                   "usb_in_us=%.3f retune_roundtrip_us=%.3f "
                   "nios_duration_valid=%u nios_duration_ticks=%llu "
                   "total_to_epoch_us=%.3f\n", i, transaction,
                   final_event.epoch_id, (out_ns - begin_ns) / 1000.0,
                   (response_ns - out_ns) / 1000.0,
                   (response_ns - begin_ns) / 1000.0,
                   nios_timing.device_duration_valid,
                   (unsigned long long)nios_timing.device_duration_ticks,
                   (final_event.host_monotonic_ns - config_ns) / 1000.0);
        } else {
            /* SPI_DONE flags carry the number of SPI writes of the tune in
             * the low bits; channel flags sit in the top bits. */
            uint32_t spi_writes = 0;
            for (uint32_t j = 0; j < count; ++j)
                if (events[j].event_type == BLADERF_RF_EVT_SPI_DONE)
                    spi_writes = events[j].flags & 0xffffu;
            printf("trial=%u mode=host txn=%u epoch=%u spi_write_us=%.3f "
                   "spi_writes=%u post_spi_to_lo_return_us=%.3f "
                   "total_to_epoch_us=%.3f\n", i, transaction,
                   final_event.epoch_id, (spi_done_ns - spi_begin_ns) / 1000.0,
                   spi_writes, (lo_return_ns - spi_done_ns) / 1000.0,
                   (final_event.host_monotonic_ns - config_ns) / 1000.0);
        }
    }

    if (trials > BLADERF_RF_EVENT_HISTORY_SIZE) {
        struct bladerf_rx_transition_nios_timing timing = {0};

        status = bladerf_rx_transition_get_nios_timing(
            dev, first_transaction_id, &timing);
        if (status != 0 || timing.transaction_retained ||
            timing.nios_retune_observed || timing.device_duration_valid ||
            timing.device_duration_ticks != 0) {
            fprintf(stderr, "old NIOS timing was not cleanly evicted: "
                    "txn=%u retained=%u observed=%u valid=%u ticks=%llu\n",
                    first_transaction_id, timing.transaction_retained,
                    timing.nios_retune_observed, timing.device_duration_valid,
                    (unsigned long long)timing.device_duration_ticks);
            goto done;
        }

        status = bladerf_rx_transition_get_nios_timing(
            dev, last_transaction_id, &timing);
        if (status != 0 || !timing.transaction_retained ||
            timing.nios_retune_observed != last_transaction_fastlock ||
            (last_transaction_fastlock && timing.device_duration_valid)) {
            fprintf(stderr, "newest NIOS timing missing after wrap: "
                    "txn=%u retained=%u observed=%u valid=%u ticks=%llu\n",
                    last_transaction_id, timing.transaction_retained,
                    timing.nios_retune_observed, timing.device_duration_valid,
                    (unsigned long long)timing.device_duration_ticks);
            goto done;
        }
        printf("timing_history oldest_txn=%u evicted=1 newest_txn=%u "
               "nios_observed=%u duration_valid=%u ticks=%llu\n",
               first_transaction_id, last_transaction_id,
               timing.nios_retune_observed, timing.device_duration_valid,
               (unsigned long long)timing.device_duration_ticks);
    }

    for (int arm = 0; arm < 2; ++arm) {
        if (arm_n[arm] == 0) continue;
        hq_print_stats(arm ? "fastlock_total_to_epoch_us" :
                             "host_total_to_epoch_us",
                       total_us[arm], arm_n[arm]);
        printf("ARM %s n=%zu to_profile0=%u to_profile1=%u\n",
               arm ? "fastlock" : "host", arm_n[arm], arm_dir[arm][0],
               arm_dir[arm][1]);
    }
    /* Informational: transitions that ran an RX RFDC (RX_RFDC_CAL_DONE in
     * their event history). Expected 0 once both input banks are seeded at
     * open (RX-TRANSITION-1S3-001). */
    printf("RFDC_TRANSITIONS n=%u first_trial=%d\n", rfdc_transitions,
           rfdc_transitions ? (int)first_rfdc_trial : -1);
    printf("PASS seed=%" PRIu64 " trials=%u\n", seed, trials);
    result = 0;
done:
    free(arm_fastlock);
    free(total_us[0]);
    free(total_us[1]);
    if (dev != NULL) {
        bladerf_enable_module(dev, RX_CHANNEL, false);
        bladerf_close(dev);
    }
    return result;
}
