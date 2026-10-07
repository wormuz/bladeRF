#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <libbladeRF.h>

#define RX1 BLADERF_CHANNEL_RX(0)
#define RX2 BLADERF_CHANNEL_RX(1)
#define SAMPLES 4096

static int require_event(struct bladerf *dev, uint32_t transaction_id,
                         bladerf_rf_event_type type, uint32_t flags)
{
    struct bladerf_rf_event events[BLADERF_RF_EVENT_HISTORY_SIZE];
    uint32_t count = 0;
    uint64_t cursor = 0;
    bool complete = false;
    int status = bladerf_rf_events_get_since(
        dev, 0, events, BLADERF_RF_EVENT_HISTORY_SIZE, &count, &cursor,
        &complete);
    if (status != 0 || !complete) {
        fprintf(stderr, "event history query failed: %s\n",
                bladerf_strerror(status));
        return -1;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (events[i].event_type == type &&
            events[i].transaction_id == transaction_id &&
            events[i].error_code == BLADERF_ERR_UNSUPPORTED &&
            events[i].flags == flags) {
            return 0;
        }
    }
    fprintf(stderr, "missing event type=%d transaction=%u flags=0x%x\n",
            type, transaction_id, flags);
    return -1;
}

static int prepare_channel(struct bladerf *dev, bladerf_channel ch)
{
    unsigned int actual_rate = 0;
    unsigned int actual_bw = 0;
    int status = bladerf_set_sample_rate(dev, ch, 1920000, &actual_rate);
    if (status == 0) {
        status = bladerf_set_bandwidth(dev, ch, 1500000, &actual_bw);
    }
    if (status != 0) {
        fprintf(stderr, "prepare channel %d: %s\n", ch,
                bladerf_strerror(status));
        return status;
    }
    return 0;
}

int main(void)
{
    struct bladerf *dev = NULL;
    int16_t samples[SAMPLES * 4] = {0};
    struct bladerf_rx_transition_request request = {0};
    struct bladerf_rf_event final = {0};
    struct bladerf_metadata meta = { .flags = BLADERF_META_FLAG_RX_NOW };
    bladerf_frequency before = 0, after = 0;
    uint32_t transaction_id = 0;
    int status = bladerf_open(&dev, NULL);
    if (status != 0) {
        fprintf(stderr, "open: %s\n", bladerf_strerror(status));
        return 1;
    }

    status = bladerf_set_tuning_mode(dev, BLADERF_TUNING_MODE_HOST);
    if (status == 0) status = bladerf_enable_module(dev, RX1, true);
    if (status == 0) status = bladerf_enable_module(dev, RX2, true);
    if (status == 0) status = prepare_channel(dev, RX1);
    if (status == 0) status = prepare_channel(dev, RX2);
    if (status != 0) goto fail;

    status = bladerf_sync_config(dev, BLADERF_RX_X1,
                                 BLADERF_FORMAT_SC16_Q11_META,
                                 8, SAMPLES, 4, 3000);
    if (status != 0) {
        fprintf(stderr, "RX_X1 sync config: %s\n", bladerf_strerror(status));
        goto fail;
    }
    status = bladerf_get_frequency(dev, RX1, &before);
    if (status != 0) goto fail;

    request.target_frequency_hz = 1835000000ULL;
    request.required_events_mask = BLADERF_RF_REQUIRE_RX_X2_HOST_DATA;
    request.require_rx_data_valid = true;
    request.timeout_ms = 3000;
    transaction_id = 0;
    status = bladerf_rx_transition_begin(dev, RX1, &request,
                                         &transaction_id);
    if (status != BLADERF_ERR_UNSUPPORTED || transaction_id != 0) {
        fprintf(stderr, "RX_X2 request with RX_X1 consumer returned %d (%s), "
                "txn=%u\n", status, bladerf_strerror(status),
                transaction_id);
        goto fail;
    }
    status = bladerf_get_frequency(dev, RX1, &after);
    if (status != 0 || after != before) {
        fprintf(stderr, "rejected layout request changed RX LO: before=%llu "
                "after=%llu status=%d\n", (unsigned long long)before,
                (unsigned long long)after, status);
        goto fail;
    }
    if (require_event(dev, 0, BLADERF_RF_EVT_RX_LAYOUT_UNSUPPORTED,
                      BLADERF_RX_X1) != 0) {
        goto fail;
    }

    status = bladerf_sync_config(dev, BLADERF_RX_X2,
                                 BLADERF_FORMAT_SC16_Q11_META,
                                 8, SAMPLES * 2, 4, 3000);
    if (status != 0) {
        fprintf(stderr, "RX_X2 sync config: %s\n", bladerf_strerror(status));
        goto fail;
    }
    status = bladerf_rx_transition_begin(dev, RX1, &request,
                                         &transaction_id);
    if (status != 0) {
        fprintf(stderr, "RX_X2 transition begin: %s\n",
                bladerf_strerror(status));
        goto fail;
    }

    /* Sync RX is the host-data consumer. Read first and retain this block;
     * transition_wait must then identify the same paired transaction. */
    memset(samples, 0, sizeof(samples));
    meta = (struct bladerf_metadata){ .flags = BLADERF_META_FLAG_RX_NOW };
    status = bladerf_sync_rx(dev, samples, SAMPLES * 2, &meta, 3000);
    if (status != 0) {
        fprintf(stderr, "first paired META read: %s\n",
                bladerf_strerror(status));
        /* Retire the transaction and preserve its terminal result. */
        (void)bladerf_rx_transition_wait(dev, transaction_id, &final, 3000);
        goto fail;
    }
    status = bladerf_rx_transition_wait(dev, transaction_id, &final, 3000);
    if (status != 0) {
        fprintf(stderr, "paired transition wait: %s\n",
                bladerf_strerror(status));
        goto fail;
    }
    if (final.event_type != BLADERF_RF_EVT_RX_FIRST_VALID_HOST_DATA ||
        !(final.flags & BLADERF_RF_EVENT_F_RX_X2_LAYOUT) ||
        final.fpga_state != BLADERF_RF_STATE_RX_DATA_VALID ||
        final.epoch_id != meta.rx_epoch_id ||
        final.fpga_timestamp != meta.timestamp ||
        meta.rx_epoch_id_valid == 0 || meta.actual_count != SAMPLES * 2 ||
        (meta.status & BLADERF_META_STATUS_OVERRUN)) {
        fprintf(stderr, "paired completion/block mismatch: event=%d flags=0x%x "
                "epoch=%u/%u timestamp=%llu/%llu count=%u status=0x%x\n",
                final.event_type, final.flags, final.epoch_id,
                meta.rx_epoch_id, (unsigned long long)final.fpga_timestamp,
                (unsigned long long)meta.timestamp, meta.actual_count,
                meta.status);
        goto fail;
    }
    printf("paired host-data PASS txn=%u epoch=%u timestamp=%llu count=%u\n",
           transaction_id, final.epoch_id,
           (unsigned long long)final.fpga_timestamp, meta.actual_count);

    status = bladerf_sync_config(dev, BLADERF_RX_X1,
                                 BLADERF_FORMAT_SC16_Q11_META,
                                 8, SAMPLES, 4, 3000);
    if (status != BLADERF_ERR_UNSUPPORTED) {
        fprintf(stderr, "RX_X1 reconfiguration during paired contract="
                "%d (%s)\n", status, bladerf_strerror(status));
        goto fail;
    }
    if (require_event(dev, transaction_id,
                      BLADERF_RF_EVT_RX_LAYOUT_UNSUPPORTED,
                      BLADERF_RX_X1) != 0) {
        goto fail;
    }

    meta = (struct bladerf_metadata){ .flags = BLADERF_META_FLAG_RX_NOW };
    status = bladerf_sync_rx(dev, samples, SAMPLES * 2, &meta, 1500);
    if (status != 0 || !meta.rx_epoch_id_valid ||
        meta.rx_epoch_id != final.epoch_id ||
        meta.actual_count != SAMPLES * 2 ||
        (meta.status & BLADERF_META_STATUS_OVERRUN) ||
        meta.timestamp <= final.fpga_timestamp) {
        fprintf(stderr, "paired stream did not survive rejected RX_X1 "
                "reconfiguration: status=%d count=%u epoch=%u timestamp=%llu\n",
                status, meta.actual_count, meta.rx_epoch_id,
                (unsigned long long)meta.timestamp);
        goto fail;
    }
    printf("paired stream remains valid after rejected RX_X1 config: "
           "epoch=%u count=%u timestamp=%llu\n", meta.rx_epoch_id,
           meta.actual_count, (unsigned long long)meta.timestamp);

    bladerf_enable_module(dev, RX1, false);
    bladerf_enable_module(dev, RX2, false);
    bladerf_close(dev);
    return 0;

fail:
    if (dev != NULL) bladerf_close(dev);
    return 2;
}
