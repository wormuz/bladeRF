#include <libbladeRF.h>
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
    const bool cross_band = argc > 3 && strcmp(argv[3], "--cross-band") == 0;
    const uint64_t frequencies[2] = {
        cross_band ? CROSS_FREQ_A : LOCAL_FREQ_A,
        cross_band ? CROSS_FREQ_B : LOCAL_FREQ_B,
    };
    struct bladerf *dev = NULL;
    struct bladerf_quick_tune profiles[2];
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
                                      16, 8192, 8, 1000)) != 0 ||
        (status = bladerf_enable_module(dev, RX_CHANNEL, true)) != 0) {
        fail_status("configure fastlock timing test", status);
        goto done;
    }

    printf("transition timing mode=%s trials=%u profiles=%u/%u frequencies=%llu/%llu\n",
           mode, trials, profiles[0].rffe_profile, profiles[1].rffe_profile,
           (unsigned long long)frequencies[0],
           (unsigned long long)frequencies[1]);

    for (unsigned i = 0; i < trials; ++i) {
        const unsigned target = (i + 1) & 1u;
        const bool use_fastlock = strcmp(mode, "fastlock") == 0 ||
                                  (strcmp(mode, "ab") == 0 && (i & 1u));
        const struct bladerf_rx_transition_request request = {
            .target_frequency_hz = frequencies[target],
            .required_events_mask = BLADERF_RF_REQUIRE_PLL_LOCKED |
                                    BLADERF_RF_REQUIRE_ENSM_RX |
                                    BLADERF_RF_REQUIRE_EPOCH_VALID,
            .timeout_ms = 2000,
            .require_rx_data_valid = true,
            .epoch_settle_samples = 0,
        };
        struct bladerf_rf_event events[BLADERF_RF_EVENT_HISTORY_SIZE];
        struct bladerf_rf_event final_event = {0};
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
        if (status == 0) {
            status = bladerf_rx_transition_get_events(
                dev, transaction, events, BLADERF_RF_EVENT_HISTORY_SIZE,
                &count, &complete);
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
        if (use_fastlock) {
            printf("trial=%u mode=fastlock txn=%u epoch=%u usb_out_us=%.3f "
                   "usb_in_us=%.3f retune_roundtrip_us=%.3f "
                   "total_to_epoch_us=%.3f\n", i, transaction,
                   final_event.epoch_id, (out_ns - begin_ns) / 1000.0,
                   (response_ns - out_ns) / 1000.0,
                   (response_ns - begin_ns) / 1000.0,
                   (final_event.host_monotonic_ns - config_ns) / 1000.0);
        } else {
            printf("trial=%u mode=host txn=%u epoch=%u spi_write_us=%.3f "
                   "post_spi_to_lo_return_us=%.3f "
                   "total_to_epoch_us=%.3f\n", i, transaction,
                   final_event.epoch_id, (spi_done_ns - spi_begin_ns) / 1000.0,
                   (lo_return_ns - spi_done_ns) / 1000.0,
                   (final_event.host_monotonic_ns - config_ns) / 1000.0);
        }
    }

    result = 0;
done:
    if (dev != NULL) {
        bladerf_enable_module(dev, RX_CHANNEL, false);
        bladerf_close(dev);
    }
    return result;
}
