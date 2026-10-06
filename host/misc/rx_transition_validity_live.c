/* Live xA4 check: request policy must control whether IQ can be valid. */
#include <libbladeRF.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(call) do { \
    status = (call); \
    if (status != 0) { \
        fprintf(stderr, "%s: %s\n", #call, bladerf_strerror(status)); \
        goto cleanup; \
    } \
} while (0)

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

int main(void)
{
    struct bladerf *dev = NULL;
    int16_t *samples = NULL;
    int status = BLADERF_ERR_UNEXPECTED;
    uint32_t txn = 0;
    struct bladerf_rf_event event = {0};
    struct bladerf_metadata metadata = {0};

    samples = calloc(8192 * 2, sizeof(*samples));
    if (samples == NULL) {
        return 2;
    }
    CHECK(bladerf_open(&dev, NULL));
    bladerf_log_set_verbosity(BLADERF_LOG_LEVEL_WARNING);
    CHECK(bladerf_set_sample_rate(dev, BLADERF_CHANNEL_RX(0), 4000000, NULL));
    CHECK(bladerf_set_bandwidth(dev, BLADERF_CHANNEL_RX(0), 5000000, NULL));
    CHECK(bladerf_set_gain(dev, BLADERF_CHANNEL_RX(0), 30));
    CHECK(bladerf_sync_config(dev, BLADERF_RX_X1, BLADERF_FORMAT_SC16_Q11_META,
                              16, 8192, 8, 1000));
    CHECK(bladerf_enable_module(dev, BLADERF_CHANNEL_RX(0), true));

    const struct bladerf_rx_transition_request control_only = {
        .target_frequency_hz = 1835000000ULL,
        .required_events_mask = BLADERF_RF_REQUIRE_PLL_LOCKED |
                                BLADERF_RF_REQUIRE_ENSM_RX,
        .timeout_ms = 2000,
        .require_rx_data_valid = false,
        .epoch_settle_samples = 0,
    };
    CHECK(bladerf_rx_transition_begin(dev, BLADERF_CHANNEL_RX(0),
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
    CHECK(bladerf_rx_transition_begin(dev, BLADERF_CHANNEL_RX(0),
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

    /* The legacy setter has no FPGA epoch confirmation. It must revoke the
     * previously certified stream until another event-driven transition. */
    CHECK(bladerf_set_frequency(dev, BLADERF_CHANNEL_RX(0), 1835500000ULL));
    metadata = (struct bladerf_metadata){0};
    metadata.flags = BLADERF_META_FLAG_RX_NOW;
    status = bladerf_sync_rx(dev, samples, 8192, &metadata, 100);
    if (status != BLADERF_ERR_WOULD_BLOCK || metadata.actual_count != 0) {
        fprintf(stderr, "legacy retune leaked uncertified IQ: status=%s count=%u\n",
                bladerf_strerror(status), metadata.actual_count);
        status = BLADERF_ERR_UNEXPECTED;
        goto cleanup;
    }

    const struct bladerf_rx_transition_request recover_valid = {
        .target_frequency_hz = 1835300000ULL,
        .required_events_mask = BLADERF_RF_REQUIRE_PLL_LOCKED,
        .timeout_ms = 2000,
        .require_rx_data_valid = true,
        .epoch_settle_samples = 0,
    };
    CHECK(bladerf_rx_transition_begin(dev, BLADERF_CHANNEL_RX(0),
                                     &recover_valid, &txn));
    CHECK(bladerf_rx_transition_wait(dev, txn, &event, 2000));
    accepted = false;
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
        fprintf(stderr, "event transition did not restore valid IQ after legacy retune\n");
        status = BLADERF_ERR_UNEXPECTED;
        goto cleanup;
    }

    printf("RX validity policy: PASS control_txn=%u legacy_retune=fenced; "
           "recovery_txn=%u epoch=%u timestamp=%llu\n",
           txn - 2, txn, event.epoch_id,
           (unsigned long long)metadata.timestamp);
    status = 0;

cleanup:
    if (dev != NULL) {
        bladerf_enable_module(dev, BLADERF_CHANNEL_RX(0), false);
        bladerf_close(dev);
    }
    free(samples);
    return status == 0 ? 0 : 1;
}
