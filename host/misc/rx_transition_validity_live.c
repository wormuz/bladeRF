/* Live xA4 check: request policy must control whether IQ can be valid. */
#include <libbladeRF.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(call) do { \
    status = (call); \
    if (status != 0) { \
        fprintf(stderr, "%s: %s\n", #call, bladerf_strerror(status)); \
        goto cleanup; \
    } \
} while (0)

static bladerf_channel rx_channel = BLADERF_CHANNEL_RX(0);

static int check_no_epoch_event(struct bladerf *dev, uint32_t txn)
{
    struct bladerf_rf_event events[BLADERF_RF_EVENT_HISTORY_SIZE];
    uint32_t count = 0;
    bool complete = false;
    int status = bladerf_rx_transition_get_events(
        dev, txn, events, BLADERF_RF_EVENT_HISTORY_SIZE, &count, &complete);
    if (status != 0 || !complete) {
        return status != 0 ? status : BLADERF_ERR_UNEXPECTED;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (events[i].event_type == BLADERF_RF_EVT_RX_EPOCH_VALID) {
            return BLADERF_ERR_UNEXPECTED;
        }
    }
    return 0;
}

static int transition_and_check_iq(struct bladerf *dev, uint64_t frequency_hz,
                                   int16_t *samples,
                                   struct bladerf_rf_event *event,
                                   uint32_t *txn)
{
    const struct bladerf_rx_transition_request request = {
        .target_frequency_hz = frequency_hz,
        .required_events_mask = BLADERF_RF_REQUIRE_PLL_LOCKED,
        .timeout_ms = 2000,
        .require_rx_data_valid = true,
        .epoch_settle_samples = 0,
    };
    int status = bladerf_rx_transition_begin(dev, rx_channel,
                                             &request, txn);
    if (status == 0) {
        status = bladerf_rx_transition_wait(dev, *txn, event, 2000);
    }
    if (status != 0) {
        return status;
    }

    for (unsigned attempt = 0; attempt < 5; ++attempt) {
        struct bladerf_metadata metadata = {0};
        metadata.flags = BLADERF_META_FLAG_RX_NOW;
        status = bladerf_sync_rx(dev, samples, 8192, &metadata, 2000);
        if (status == 0 && metadata.rx_epoch_id_valid &&
            metadata.rx_epoch_id == event->epoch_id &&
            metadata.actual_count == 8192 &&
            (metadata.status & BLADERF_META_STATUS_OVERRUN) == 0 &&
            metadata.timestamp >= event->fpga_timestamp) {
            return 0;
        }
    }

    fprintf(stderr, "event transition did not restore valid IQ at %llu Hz\n",
            (unsigned long long)frequency_hz);
    return BLADERF_ERR_UNEXPECTED;
}

static int latest_rf_event_cursor(struct bladerf *dev, uint64_t *cursor)
{
    struct bladerf_rf_event events[BLADERF_RF_EVENT_HISTORY_SIZE];
    uint32_t count = 0;
    uint64_t next_sequence = 0;
    bool history_complete = false;
    int status = bladerf_rf_events_get_since(
        dev, 0, events, BLADERF_RF_EVENT_HISTORY_SIZE, &count,
        &next_sequence, &history_complete);
    /* A busy device may append while the snapshot is copied. The returned
     * cursor is still the last copied event, a valid boundary for our query. */
    if (status != 0 && status != BLADERF_ERR_MEM) {
        return status;
    }
    if (count == 0 && !history_complete) {
        return BLADERF_ERR_UNEXPECTED;
    }
    *cursor = next_sequence;
    return 0;
}

static int check_invalidation_reason(struct bladerf *dev, uint64_t cursor,
                                    uint32_t reason)
{
    struct bladerf_rf_event events[BLADERF_RF_EVENT_HISTORY_SIZE];
    uint32_t count = 0;
    uint64_t next_sequence = cursor;
    bool history_complete = false;
    int status = bladerf_rf_events_get_since(
        dev, cursor, events, BLADERF_RF_EVENT_HISTORY_SIZE, &count,
        &next_sequence, &history_complete);
    if (status != 0 && status != BLADERF_ERR_MEM) {
        return status;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (events[i].event_type == BLADERF_RF_EVT_RX_DATA_INVALIDATED &&
            events[i].flags == reason) {
            return 0;
        }
    }
    fprintf(stderr, "missing RX_DATA_INVALIDATED reason 0x%x\n", reason);
    return BLADERF_ERR_UNEXPECTED;
}

static int assert_rx_iq_withheld(struct bladerf *dev, int16_t *samples,
                                 const char *operation)
{
    struct bladerf_metadata metadata = {0};
    metadata.flags = BLADERF_META_FLAG_RX_NOW;
    const int status = bladerf_sync_rx(dev, samples, 8192, &metadata, 100);
    if (status != BLADERF_ERR_WOULD_BLOCK || metadata.actual_count != 0) {
        fprintf(stderr, "%s left RX IQ valid: status=%s count=%u\n",
                operation, bladerf_strerror(status), metadata.actual_count);
        return BLADERF_ERR_UNEXPECTED;
    }
    return 0;
}

static int check_sync_timeout_timestamp_event(struct bladerf *dev,
                                              int16_t *samples,
                                              uint8_t expected_epoch_id,
                                              uint64_t epoch_start)
{
    struct bladerf_metadata metadata = {0};
    struct bladerf_rf_event events[BLADERF_RF_EVENT_HISTORY_SIZE];
    uint32_t count = 0;
    uint64_t cursor = 0;
    uint64_t next = 0;
    bladerf_timestamp now = 0;
    bool complete = false;
    int status = bladerf_rf_events_get_since(
        dev, 0, events, BLADERF_RF_EVENT_HISTORY_SIZE, &count, &cursor,
        &complete);
    if (status != 0 || !complete) {
        return status != 0 ? status : BLADERF_ERR_UNEXPECTED;
    }
    status = bladerf_get_timestamp(dev, BLADERF_RX, &now);
    if (status != 0) {
        return status;
    }

    /* Ask for a sample coordinate well beyond this short watchdog window.
     * Timeout must fail the read while reporting the parser's last known
     * coordinate; it cannot turn the requested future timestamp into IQ. */
    metadata.timestamp = now + 4000000ULL;
    status = bladerf_sync_rx(dev, samples, 8192, &metadata, 20);
    if (status != BLADERF_ERR_TIMEOUT || metadata.actual_count != 0) {
        fprintf(stderr, "future-timestamp read was not fail-closed: %s count=%u\n",
                bladerf_strerror(status), metadata.actual_count);
        return BLADERF_ERR_UNEXPECTED;
    }

    status = bladerf_rf_events_get_since(
        dev, cursor, events, BLADERF_RF_EVENT_HISTORY_SIZE, &count, &next,
        &complete);
    if (status != 0 || !complete) {
        return status != 0 ? status : BLADERF_ERR_UNEXPECTED;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (events[i].event_type == BLADERF_RF_EVT_RX_DATA_WITHHELD &&
            (events[i].flags & ~BLADERF_RF_EVENT_F_FPGA_TIMESTAMP_VALID) ==
                BLADERF_RF_WITHHELD_SYNC_TIMEOUT) {
            if ((events[i].flags &
                 BLADERF_RF_EVENT_F_FPGA_TIMESTAMP_VALID) == 0 ||
                events[i].fpga_timestamp < epoch_start ||
                events[i].epoch_id != expected_epoch_id) {
                fprintf(stderr, "sync timeout event has no current timestamp "
                        "boundary: epoch=%u timestamp=%llu valid=%u\n",
                        events[i].epoch_id,
                        (unsigned long long)events[i].fpga_timestamp,
                        (events[i].flags &
                         BLADERF_RF_EVENT_F_FPGA_TIMESTAMP_VALID) != 0);
                return BLADERF_ERR_UNEXPECTED;
            }
            printf("sync timeout boundary: epoch=%u timestamp=%llu\n",
                   events[i].epoch_id,
                   (unsigned long long)events[i].fpga_timestamp);
            return 0;
        }
    }
    fprintf(stderr, "sync timeout event was not recorded\n");
    return BLADERF_ERR_UNEXPECTED;
}

int main(int argc, char **argv)
{
    struct bladerf *dev = NULL;
    int16_t *samples = NULL;
    int status = BLADERF_ERR_UNEXPECTED;
    uint32_t txn = 0;
    struct bladerf_rf_event event = {0};
    struct bladerf_metadata metadata = {0};
    uint64_t tx_fir_event_cursor = 0;
    bladerf_channel rx2_channel = BLADERF_CHANNEL_RX(1);
    bladerf_channel_layout layout = BLADERF_RX_X1;
    bool paired = false;

    if (argc > 2 || (argc == 2 && strcmp(argv[1], "RX1") != 0 &&
                     strcmp(argv[1], "RX2") != 0 &&
                     strcmp(argv[1], "BOTH") != 0)) {
        fprintf(stderr, "usage: %s [RX1|RX2|BOTH]\n", argv[0]);
        return 2;
    }
    if (argc == 2 && strcmp(argv[1], "RX2") == 0) {
        rx_channel = rx2_channel;
    } else if (argc == 2 && strcmp(argv[1], "BOTH") == 0) {
        layout = BLADERF_RX_X2;
        paired = true;
    }

    samples = calloc(8192 * 2, sizeof(*samples));
    if (samples == NULL) {
        return 2;
    }
    CHECK(bladerf_open(&dev, NULL));
    bladerf_log_set_verbosity(BLADERF_LOG_LEVEL_WARNING);
    CHECK(bladerf_set_sample_rate(dev, rx_channel, 4000000, NULL));
    CHECK(bladerf_set_bandwidth(dev, rx_channel, 5000000, NULL));
    CHECK(bladerf_set_gain(dev, rx_channel, 30));
    CHECK(bladerf_sync_config(dev, layout, BLADERF_FORMAT_SC16_Q11_META,
                              16, 8192, 8, 1000));
    CHECK(bladerf_enable_module(dev, rx_channel, true));
    if (paired) {
        CHECK(bladerf_enable_module(dev, rx2_channel, true));
    }

    const struct bladerf_rx_transition_request control_only = {
        .target_frequency_hz = 1835000000ULL,
        .required_events_mask = BLADERF_RF_REQUIRE_PLL_LOCKED |
                                BLADERF_RF_REQUIRE_ENSM_RX,
        .timeout_ms = 2000,
        .require_rx_data_valid = false,
        .epoch_settle_samples = 0,
    };
    CHECK(bladerf_rx_transition_begin(dev, rx_channel,
                                     &control_only, &txn));
    CHECK(bladerf_rx_transition_wait(dev, txn, &event, 2000));
    if (event.event_type != BLADERF_RF_EVT_CONTROL_PLANE_CONFIRMED ||
        event.fpga_state != BLADERF_RF_STATE_RX_DATA_INVALID) {
        fprintf(stderr, "control-only result claimed wrong state/event: %u/%u\n",
                event.fpga_state, event.event_type);
        status = BLADERF_ERR_UNEXPECTED;
        goto cleanup;
    }
    CHECK(check_no_epoch_event(dev, txn));

    /* The mask omits EPOCH_VALID on purpose. The boolean must promote it. */
    const struct bladerf_rx_transition_request require_valid = {
        .target_frequency_hz = 1835400000ULL,
        .required_events_mask = BLADERF_RF_REQUIRE_PLL_LOCKED,
        .timeout_ms = 2000,
        .require_rx_data_valid = true,
        .epoch_settle_samples = 0,
    };
    CHECK(bladerf_rx_transition_begin(dev, rx_channel,
                                     &require_valid, &txn));
    CHECK(bladerf_rx_transition_wait(dev, txn, &event, 2000));
    if (event.event_type != BLADERF_RF_EVT_RX_EPOCH_VALID ||
        event.fpga_state != BLADERF_RF_STATE_RX_DATA_VALID ||
        event.epoch_id == 0) {
        fprintf(stderr, "data-valid request did not return a valid epoch\n");
        status = BLADERF_ERR_UNEXPECTED;
        goto cleanup;
    }

    bool accepted = false;
    for (unsigned attempt = 0; attempt < 5 && !accepted; ++attempt) {
        metadata = (struct bladerf_metadata){0};
        metadata.flags = BLADERF_META_FLAG_RX_NOW;
        status = bladerf_sync_rx(dev, samples, 8192, &metadata, 2000);
        accepted = status == 0 && metadata.rx_epoch_id_valid &&
                   metadata.rx_epoch_id == event.epoch_id &&
                   metadata.actual_count == 8192 &&
                   (metadata.status & BLADERF_META_STATUS_OVERRUN) == 0 &&
                   metadata.timestamp >= event.fpga_timestamp;
    }
    if (!accepted) {
        fprintf(stderr, "no clean IQ block for event epoch=%u: status=%s "
                "meta_epoch=%u valid=%u count=%u flags=0x%x ts=%llu boundary=%llu\n",
                event.epoch_id, bladerf_strerror(status), metadata.rx_epoch_id,
                metadata.rx_epoch_id_valid, metadata.actual_count,
                metadata.status, (unsigned long long)metadata.timestamp,
                (unsigned long long)event.fpga_timestamp);
        status = BLADERF_ERR_UNEXPECTED;
        goto cleanup;
    }

    CHECK(check_sync_timeout_timestamp_event(dev, samples, event.epoch_id,
                                              event.fpga_timestamp));

    /* The legacy setter has no FPGA epoch confirmation. It must revoke the
     * previously certified stream until another event-driven transition. */
    CHECK(bladerf_set_frequency(dev, rx_channel, 1835500000ULL));
    metadata = (struct bladerf_metadata){0};
    metadata.flags = BLADERF_META_FLAG_RX_NOW;
    status = bladerf_sync_rx(dev, samples, 8192, &metadata, 100);
    if (status != BLADERF_ERR_WOULD_BLOCK || metadata.actual_count != 0) {
        fprintf(stderr, "legacy retune leaked uncertified IQ: status=%s count=%u\n",
                bladerf_strerror(status), metadata.actual_count);
        status = BLADERF_ERR_UNEXPECTED;
        goto cleanup;
    }

    CHECK(transition_and_check_iq(dev, 1835300000ULL, samples, &event, &txn));

    /* Bandwidth and sample-rate changes also invalidate the old datapath
     * certificate; each must require a fresh event transition. */
    CHECK(bladerf_set_bandwidth(dev, rx_channel, 4500000, NULL));
    metadata = (struct bladerf_metadata){0};
    metadata.flags = BLADERF_META_FLAG_RX_NOW;
    status = bladerf_sync_rx(dev, samples, 8192, &metadata, 100);
    if (status != BLADERF_ERR_WOULD_BLOCK || metadata.actual_count != 0) {
        fprintf(stderr, "bandwidth change left RX epoch certified: %s count=%u\n",
                bladerf_strerror(status), metadata.actual_count);
        status = BLADERF_ERR_UNEXPECTED;
        goto cleanup;
    }
    CHECK(transition_and_check_iq(dev, 1835300000ULL, samples, &event, &txn));

    CHECK(bladerf_set_sample_rate(dev, rx_channel, 3840000, NULL));
    CHECK(assert_rx_iq_withheld(dev, samples, "sample-rate change"));
    CHECK(transition_and_check_iq(dev, 1835300000ULL, samples, &event, &txn));

    /* ADI documents TX FIR programming as potentially data-path changing.
     * It must revoke shared RX validity and publish a specific reason. */
    CHECK(latest_rf_event_cursor(dev, &tx_fir_event_cursor));
    CHECK(bladerf_set_rfic_tx_fir(dev, BLADERF_RFIC_TXFIR_DEFAULT));
    CHECK(check_invalidation_reason(dev, tx_fir_event_cursor,
                                    BLADERF_RF_INVALIDATE_TX_FIR));
    CHECK(assert_rx_iq_withheld(dev, samples, "TX FIR change"));
    CHECK(transition_and_check_iq(dev, 1835300000ULL, samples, &event, &txn));

    /* The public low-level GPIO API writes the whole register, including
     * clock-select and RX-mux bits. Writing back the observed value is benign
     * electrically but must still revoke opaque configuration's old epoch. */
    uint32_t config_gpio = 0;
    CHECK(latest_rf_event_cursor(dev, &tx_fir_event_cursor));
    CHECK(bladerf_config_gpio_read(dev, &config_gpio));
    CHECK(bladerf_config_gpio_write(dev, config_gpio));
    CHECK(check_invalidation_reason(dev, tx_fir_event_cursor,
                                    BLADERF_RF_INVALIDATE_CONFIG_GPIO));
    CHECK(assert_rx_iq_withheld(dev, samples, "config GPIO write"));
    CHECK(transition_and_check_iq(dev, 1835300000ULL, samples, &event, &txn));

    printf("RX validity policy: PASS mode=%s legacy_LO/BW/rate/TX_FIR/"
           "config_GPIO=fenced; final_txn=%u epoch=%u\n",
           paired ? "BOTH" : (rx_channel == rx2_channel ? "RX2" : "RX1"),
           txn, event.epoch_id);
    status = 0;

cleanup:
    if (dev != NULL) {
        bladerf_enable_module(dev, rx_channel, false);
        if (paired) {
            bladerf_enable_module(dev, rx2_channel, false);
        }
        bladerf_close(dev);
    }
    free(samples);
    return status == 0 ? 0 : 1;
}
