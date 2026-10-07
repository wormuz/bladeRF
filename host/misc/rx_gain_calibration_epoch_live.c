#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libbladeRF.h>

/* Live xA4 regression: gain-calibration loading must revoke an existing RX
 * epoch, and the next event transition must restore admitted META IQ. Pass a
 * writable CSV path because the loader creates a sibling .tbl file. */

#define CHECK(call) do { int s_ = (call); if (s_ != 0) { \
    fprintf(stderr, "%s: %s\n", #call, bladerf_strerror(s_)); goto fail; } } while (0)

static int transition(struct bladerf *dev, bladerf_channel ch, uint32_t *txn,
                      struct bladerf_rf_event *event)
{
    struct bladerf_rx_transition_request req = {
        .target_frequency_hz = 1835000000ULL,
        .required_events_mask = BLADERF_RF_REQUIRE_PLL_LOCKED |
                                BLADERF_RF_REQUIRE_ENSM_RX |
                                BLADERF_RF_REQUIRE_EPOCH_VALID,
        .timeout_ms = 1000,
        .require_rx_data_valid = true,
        .epoch_settle_samples = 0,
    };
    int status = bladerf_rx_transition_begin(dev, ch,
                                               &req, txn);
    if (status != 0) return status;
    return bladerf_rx_transition_wait(dev, *txn, event, req.timeout_ms);
}

int main(int argc, char **argv)
{
    struct bladerf *dev = NULL;
    struct bladerf_rf_event event;
    struct bladerf_metadata meta = {0};
    int16_t *samples = NULL;
    uint32_t txn = 0;
    bladerf_channel ch;
    int status;
    if (argc != 3 ||
        (strcmp(argv[1], "RX1") != 0 && strcmp(argv[1], "RX2") != 0)) {
        fprintf(stderr, "usage: %s RX1|RX2 writable-calibration.csv\n", argv[0]);
        return 2;
    }
    ch = BLADERF_CHANNEL_RX(strcmp(argv[1], "RX2") == 0 ? 1 : 0);
    CHECK(bladerf_open(&dev, NULL));
    CHECK(bladerf_sync_config(dev, BLADERF_RX_X1, BLADERF_FORMAT_SC16_Q11_META,
                              16, 8192, 8, 1000));
    CHECK(bladerf_enable_module(dev, ch, true));
    CHECK(transition(dev, ch, &txn, &event));
    samples = calloc(8192 * 2, sizeof(*samples));
    if (!samples) goto fail;

    status = bladerf_load_gain_calibration(dev, ch, argv[2]);
    if (status != 0) {
        fprintf(stderr, "load calibration: %s\n", bladerf_strerror(status));
        goto fail;
    }
    meta.flags = BLADERF_META_FLAG_RX_NOW;
    status = bladerf_sync_rx(dev, samples, 8192, &meta, 20);
    if (status != BLADERF_ERR_WOULD_BLOCK || meta.actual_count != 0) {
        fprintf(stderr, "post-load read was not fenced: status=%s count=%u\n",
                bladerf_strerror(status), meta.actual_count);
        goto fail;
    }
    CHECK(transition(dev, ch, &txn, &event));
    meta = (struct bladerf_metadata){0};
    meta.flags = BLADERF_META_FLAG_RX_NOW;
    status = BLADERF_ERR_WOULD_BLOCK;
    for (unsigned attempt = 0; attempt < 5 && status != 0; ++attempt)
        status = bladerf_sync_rx(dev, samples, 8192, &meta, 1000);
    if (status != 0 || meta.actual_count != 8192 || !meta.rx_epoch_id_valid ||
        meta.rx_epoch_id != event.epoch_id ||
        meta.timestamp < event.fpga_timestamp) {
        fprintf(stderr, "recovery metadata mismatch: count=%u epoch=%u/%u ts=%llu/%llu\n",
                meta.actual_count, meta.rx_epoch_id, event.epoch_id,
                (unsigned long long)meta.timestamp,
                (unsigned long long)event.fpga_timestamp);
        goto fail;
    }
    printf("%s gain-calibration load fence: PASS epoch=%u timestamp=%llu\n",
           argv[1], event.epoch_id, (unsigned long long)meta.timestamp);
    free(samples);
    bladerf_close(dev);
    return 0;
fail:
    free(samples);
    if (dev) bladerf_close(dev);
    return 1;
}
