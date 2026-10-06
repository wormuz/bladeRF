#include <libbladeRF.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define RX_CHANNEL BLADERF_CHANNEL_RX(0)
#define RX_SAMPLE_RATE 4000000
#define RX_BANDWIDTH 5000000
#define FREQ_A 1835000000ULL
#define FREQ_B 1835400000ULL

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
    struct bladerf *dev = NULL;
    struct bladerf_quick_tune profiles[2];
    int status;
    int result = 1;

    if (trials == 0) {
        fputs("trial count must be positive\n", stderr);
        return 2;
    }
    status = bladerf_open(&dev, NULL);
    if (status != 0) return fail_status("open", status);

    if ((status = bladerf_set_sample_rate(dev, RX_CHANNEL, RX_SAMPLE_RATE, NULL)) != 0 ||
        (status = bladerf_set_bandwidth(dev, RX_CHANNEL, RX_BANDWIDTH, NULL)) != 0 ||
        (status = bladerf_set_frequency(dev, RX_CHANNEL, FREQ_A)) != 0 ||
        (status = bladerf_get_quick_tune(dev, RX_CHANNEL, &profiles[0])) != 0 ||
        (status = bladerf_set_frequency(dev, RX_CHANNEL, FREQ_B)) != 0 ||
        (status = bladerf_get_quick_tune(dev, RX_CHANNEL, &profiles[1])) != 0 ||
        (status = bladerf_sync_config(dev, BLADERF_RX_X1,
                                      BLADERF_FORMAT_SC16_Q11_META,
                                      16, 8192, 8, 1000)) != 0 ||
        (status = bladerf_enable_module(dev, RX_CHANNEL, true)) != 0) {
        fail_status("configure fastlock timing test", status);
        goto done;
    }

    printf("fastlock timing trials=%u profiles=%u/%u frequencies=%llu/%llu\n",
           trials, profiles[0].rffe_profile, profiles[1].rffe_profile,
           (unsigned long long)FREQ_A, (unsigned long long)FREQ_B);

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
        struct bladerf_rf_event events[BLADERF_RF_EVENT_HISTORY_SIZE];
        struct bladerf_rf_event final_event = {0};
        uint32_t transaction = 0, count = 0;
        bool complete = false;
        uint64_t begin_ns = 0, out_ns = 0, response_ns = 0;

        status = bladerf_rx_transition_begin_quick_tune(
            dev, RX_CHANNEL, &request, &profiles[target], &transaction);
        if (status == 0) {
            status = bladerf_rx_transition_wait(dev, transaction, &final_event,
                                                request.timeout_ms);
        }
        if (status == 0) {
            status = bladerf_rx_transition_get_events(
                dev, transaction, events, BLADERF_RF_EVENT_HISTORY_SIZE,
                &count, &complete);
        }
        if (status != 0 || !complete ||
            event_time(events, count, BLADERF_RF_EVT_NIOS_RETUNE_BEGIN,
                       &begin_ns) != 0 ||
            event_time(events, count, BLADERF_RF_EVT_NIOS_RETUNE_USB_OUT_DONE,
                       &out_ns) != 0 ||
            event_time(events, count, BLADERF_RF_EVT_NIOS_RETUNE_RESPONSE,
                       &response_ns) != 0 ||
            begin_ns > out_ns || out_ns > response_ns ||
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
        printf("trial=%u txn=%u epoch=%u usb_out_us=%.3f usb_in_us=%.3f "
               "retune_roundtrip_us=%.3f total_to_epoch_us=%.3f\n",
               i, transaction, final_event.epoch_id,
               (out_ns - begin_ns) / 1000.0,
               (response_ns - out_ns) / 1000.0,
               (response_ns - begin_ns) / 1000.0,
               (final_event.host_monotonic_ns - begin_ns) / 1000.0);
    }

    result = 0;
done:
    if (dev != NULL) {
        bladerf_enable_module(dev, RX_CHANNEL, false);
        bladerf_close(dev);
    }
    return result;
}
