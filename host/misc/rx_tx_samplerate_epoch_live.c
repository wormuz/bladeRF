#define _DEFAULT_SOURCE
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libbladeRF.h>

#define RX_RATE_BEFORE 1920000u
#define RX_RATE_AFTER  2000000u
#define RX_BANDWIDTH   1500000u
#define RX_FREQUENCY   1835000000ULL
#define RX_SAMPLES     4096u

static int fail(const char *what, int status)
{
    fprintf(stderr, "%s: %s (%d)\n", what, bladerf_strerror(status), status);
    return 1;
}

static int transition(struct bladerf *dev, bladerf_channel ch,
                      struct bladerf_rf_event *final_event)
{
    const struct bladerf_rx_transition_request request = {
        .target_frequency_hz = RX_FREQUENCY,
        .required_events_mask = BLADERF_RF_REQUIRE_PLL_LOCKED |
                                BLADERF_RF_REQUIRE_ENSM_RX |
                                BLADERF_RF_REQUIRE_EPOCH_VALID,
        .timeout_ms = 2000,
        .require_rx_data_valid = true,
        .epoch_settle_samples = 0,
    };
    uint32_t transaction_id = 0;
    int status = bladerf_rx_transition_begin(
        dev, ch, &request, &transaction_id);
    if (status != 0) {
        return status;
    }
    return bladerf_rx_transition_wait(dev, transaction_id, final_event, 2000);
}

int main(int argc, char **argv)
{
    struct bladerf *dev = NULL;
    struct bladerf_rf_event events[64];
    struct bladerf_rf_event final_event = {0};
    struct bladerf_metadata metadata = { .flags = BLADERF_META_FLAG_RX_NOW };
    const bladerf_channel ch = argc > 1 && strcmp(argv[1], "RX2") == 0
        ? BLADERF_CHANNEL_RX(1) : BLADERF_CHANNEL_RX(0);
    const char *channel_name = ch == BLADERF_CHANNEL_RX(0) ? "RX1" : "RX2";
    int16_t samples[RX_SAMPLES * 2];
    uint32_t event_count = 0;
    uint64_t event_cursor = 0;
    uint64_t next_sequence = 0;
    bladerf_sample_rate actual_rate = 0;
    bladerf_sample_rate rx_rate = 0;
    bool history_complete = false;
    bool saw_rate_invalidation = false;
    int status;

    if (argc > 2 || (argc == 2 && strcmp(argv[1], "RX1") != 0 &&
                                   strcmp(argv[1], "RX2") != 0)) {
        fprintf(stderr, "usage: %s [RX1|RX2]\n", argv[0]);
        return 2;
    }

    status = bladerf_open(&dev, NULL);
    if (status != 0) return fail("open", status);
    status = bladerf_set_sample_rate(dev, ch, RX_RATE_BEFORE, &actual_rate);
    if (status != 0) { fail("set initial RX rate", status); goto error; }
    status = bladerf_set_bandwidth(dev, ch, RX_BANDWIDTH, NULL);
    if (status != 0) { fail("set RX bandwidth", status); goto error; }
    status = bladerf_set_frequency(dev, ch, RX_FREQUENCY);
    if (status != 0) { fail("set RX frequency", status); goto error; }
    status = bladerf_sync_config(dev, BLADERF_RX_X1,
                                 BLADERF_FORMAT_SC16_Q11_META,
                                 8, RX_SAMPLES, 4, 1000);
    if (status != 0) { fail("sync config", status); goto error; }
    status = bladerf_enable_module(dev, ch, true);
    if (status != 0) { fail("enable RX", status); goto error; }

    status = transition(dev, ch, &final_event);
    if (status != 0) { fail("initial event transition", status); goto error; }
    status = bladerf_rf_events_get_since(dev, 0, events, 64, &event_count,
                                         &next_sequence, &history_complete);
    if (status != 0 || !history_complete) {
        if (status == 0) status = BLADERF_ERR_UNEXPECTED;
        fail("read event cursor", status); goto error;
    }
    event_cursor = next_sequence;

    /* TX rate configuration calls AD9361's shared RX/TX clock-chain
     * calculator. It must revoke this active RX epoch before the RFIC write. */
    status = bladerf_set_sample_rate(dev, BLADERF_CHANNEL_TX(0),
                                     RX_RATE_AFTER, &actual_rate);
    if (status != 0) { fail("set TX sample rate", status); goto error; }
    status = bladerf_get_sample_rate(dev, ch, &rx_rate);
    if (status != 0) { fail("read RX sample rate after TX change", status); goto error; }
    if (rx_rate != RX_RATE_AFTER) {
        fprintf(stderr, "shared clock evidence mismatch: RX rate=%u expected=%u\n",
                rx_rate, RX_RATE_AFTER);
        goto error;
    }

    event_count = 0;
    status = bladerf_rf_events_get_since(dev, event_cursor, events, 64,
                                         &event_count, &next_sequence,
                                         &history_complete);
    if (status != 0 || !history_complete) {
        if (status == 0) status = BLADERF_ERR_UNEXPECTED;
        fail("read TX-rate invalidation event", status); goto error;
    }
    for (uint32_t i = 0; i < event_count; ++i) {
        if (events[i].event_type == BLADERF_RF_EVT_RX_DATA_INVALIDATED &&
            events[i].flags == BLADERF_RF_INVALIDATE_SAMPLE_RATE) {
            saw_rate_invalidation = true;
        }
    }
    if (!saw_rate_invalidation) {
        fprintf(stderr, "TX rate change emitted no RX sample-rate invalidation\n");
        goto error;
    }

    memset(samples, 0xA5, sizeof(samples));
    metadata = (struct bladerf_metadata){ .flags = BLADERF_META_FLAG_RX_NOW };
    status = bladerf_sync_rx(dev, samples, RX_SAMPLES, &metadata, 100);
    if (status != BLADERF_ERR_WOULD_BLOCK || metadata.actual_count != 0 ||
        samples[0] != (int16_t)0xA5A5) {
        fprintf(stderr, "invalid RX escaped TX-rate change: status=%s count=%u\n",
                bladerf_strerror(status), metadata.actual_count);
        goto error;
    }

    status = transition(dev, ch, &final_event);
    if (status != 0) { fail("recovery event transition", status); goto error; }
    for (unsigned int attempt = 0; attempt < 30; ++attempt) {
        metadata = (struct bladerf_metadata){ .flags = BLADERF_META_FLAG_RX_NOW };
        status = bladerf_sync_rx(dev, samples, RX_SAMPLES, &metadata, 500);
        if (status == 0) break;
        if (status != BLADERF_ERR_WOULD_BLOCK) {
            fail("recovered sync RX", status); goto error;
        }
        usleep(10000);
    }
    if (status != 0 || metadata.actual_count != RX_SAMPLES ||
        !metadata.rx_epoch_id_valid ||
        metadata.rx_epoch_id != final_event.epoch_id ||
        metadata.timestamp < final_event.fpga_timestamp) {
        fprintf(stderr, "recovery META mismatch: status=%s count=%u epoch=%u/%u "
                "timestamp=%" PRIu64 " boundary=%" PRIu64 "\n",
                bladerf_strerror(status), metadata.actual_count,
                metadata.rx_epoch_id, final_event.epoch_id,
                metadata.timestamp, final_event.fpga_timestamp);
        goto error;
    }

    printf("TX shared-clock invalidation PASS channel=%s tx_rate=%u rx_rate=%u "
           "epoch=%u first_timestamp=%" PRIu64 " boundary=%" PRIu64 "\n",
           channel_name, actual_rate, rx_rate, metadata.rx_epoch_id,
           metadata.timestamp, final_event.fpga_timestamp);
    (void)bladerf_enable_module(dev, ch, false);
    bladerf_close(dev);
    return 0;

error:
    if (dev != NULL) {
        (void)bladerf_enable_module(dev, ch, false);
        bladerf_close(dev);
    }
    return 1;
}
