#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <libbladeRF.h>

static int transition(struct bladerf *dev)
{
    struct bladerf_rx_transition_request req = {0};
    struct bladerf_rf_event final = {0};
    uint32_t id = 0;
    int s;
    req.target_frequency_hz = 1835000000ULL;
    req.required_events_mask = BLADERF_RF_REQUIRE_EPOCH_VALID;
    req.require_rx_data_valid = true;
    req.timeout_ms = 3000;
    s = bladerf_rx_transition_begin(dev, BLADERF_CHANNEL_RX(0), &req, &id);
    if (s != 0) return s;
    s = bladerf_rx_transition_wait(dev, id, &final, 3000);
    if (s == 0) {
        printf("transition id=%u state=%d epoch=%u\n", id,
               final.fpga_state, final.epoch_id);
        if (final.fpga_state != BLADERF_RF_STATE_RX_DATA_VALID) {
            return BLADERF_ERR_UNEXPECTED;
        }
    }
    return s;
}

int main(void)
{
    struct bladerf *dev = NULL;
    struct bladerf_metadata meta = { .flags = BLADERF_META_FLAG_RX_NOW };
    int16_t samples[4096 * 2];
    int s, raw_status;
    s = bladerf_open(&dev, NULL);
    if (s != 0) { fprintf(stderr, "open: %s\n", bladerf_strerror(s)); return 1; }
    s = bladerf_enable_module(dev, BLADERF_CHANNEL_RX(0), true);
    if (s != 0) { fprintf(stderr, "enable: %s\n", bladerf_strerror(s)); goto fail; }
    s = transition(dev);
    if (s != 0) { fprintf(stderr, "first transition: %s\n", bladerf_strerror(s)); goto fail; }
    raw_status = bladerf_sync_config(dev, BLADERF_RX_X1,
                                     BLADERF_FORMAT_SC16_Q11, 8, 4096, 4, 3000);
    printf("raw sync config status=%d (%s), expected %d\n", raw_status,
           bladerf_strerror(raw_status), BLADERF_ERR_UNSUPPORTED);
    if (raw_status != BLADERF_ERR_UNSUPPORTED) goto fail;
    s = bladerf_sync_config(dev, BLADERF_RX_X1,
                            BLADERF_FORMAT_SC16_Q11_META, 8, 4096, 4, 3000);
    if (s != 0) { fprintf(stderr, "META sync config: %s\n", bladerf_strerror(s)); goto fail; }
    s = bladerf_sync_rx(dev, samples, 4096, &meta, 1000);
    printf("pre-transition sync RX status=%d (%s), actual_count=%u\n", s,
           bladerf_strerror(s), meta.actual_count);
    if (s != BLADERF_ERR_WOULD_BLOCK) goto fail;
    s = transition(dev);
    if (s != 0) { fprintf(stderr, "recovery transition: %s\n", bladerf_strerror(s)); goto fail; }
    meta = (struct bladerf_metadata){ .flags = BLADERF_META_FLAG_RX_NOW };
    for (unsigned int attempt = 0; attempt < 30; ++attempt) {
        s = bladerf_sync_rx(dev, samples, 4096, &meta, 500);
        if (s == 0) break;
        if (s != BLADERF_ERR_WOULD_BLOCK) {
            fprintf(stderr, "post-transition sync RX: %s\n", bladerf_strerror(s));
            goto fail;
        }
        usleep(10000);
    }
    if (s != 0) { fprintf(stderr, "post-transition sync RX never delivered valid block: %s\n", bladerf_strerror(s)); goto fail; }
    printf("post-transition META RX status=0 samples=%u epoch_valid=%d epoch=%u timestamp=%llu\n",
           meta.actual_count, meta.rx_epoch_id_valid, meta.rx_epoch_id,
           (unsigned long long)meta.timestamp);
    if (!meta.rx_epoch_id_valid || meta.actual_count != 4096) goto fail;
    bladerf_enable_module(dev, BLADERF_CHANNEL_RX(0), false);
    bladerf_close(dev);
    return 0;
fail:
    if (dev != NULL) bladerf_close(dev);
    return 2;
}
