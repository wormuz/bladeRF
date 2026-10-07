#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <libbladeRF.h>

static bladerf_channel rx_channel = BLADERF_CHANNEL_RX(0);
static bladerf_channel_layout rx_layout = BLADERF_RX_X1;

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
    s = bladerf_rx_transition_begin(dev, rx_channel, &req, &id);
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

int main(int argc, char **argv)
{
    struct bladerf *dev = NULL;
    struct bladerf_metadata meta = { .flags = BLADERF_META_FLAG_RX_NOW };
    struct bladerf_rf_event events[BLADERF_RF_EVENT_HISTORY_SIZE];
    uint64_t event_cursor = 0, next_event_cursor = 0;
    uint32_t event_count = 0;
    bool history_complete = false;
    unsigned int format_event_count = 0;
    int16_t samples[4096 * 2];
    int s, raw_status;
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "RX1") != 0 &&
                                   strcmp(argv[1], "RX2") != 0)) {
        fprintf(stderr, "usage: %s [RX1|RX2]\n", argv[0]);
        return 1;
    }
    if (argc == 2 && strcmp(argv[1], "RX2") == 0) {
        rx_channel = BLADERF_CHANNEL_RX(1);
    }
    s = bladerf_open(&dev, NULL);
    if (s != 0) { fprintf(stderr, "open: %s\n", bladerf_strerror(s)); return 1; }
    s = bladerf_enable_module(dev, rx_channel, true);
    if (s != 0) { fprintf(stderr, "enable: %s\n", bladerf_strerror(s)); goto fail; }
    s = transition(dev);
    if (s != 0) { fprintf(stderr, "first transition: %s\n", bladerf_strerror(s)); goto fail; }
    s = bladerf_rf_events_get_since(dev, 0, events,
                                    BLADERF_RF_EVENT_HISTORY_SIZE,
                                    &event_count, &event_cursor,
                                    &history_complete);
    if (s != 0 || !history_complete) {
        fprintf(stderr, "event cursor query failed: %s\n", bladerf_strerror(s));
        goto fail;
    }
    raw_status = bladerf_sync_config(dev, rx_layout,
                                     BLADERF_FORMAT_SC16_Q11, 8, 4096, 4, 3000);
    printf("raw sync config status=%d (%s), expected %d\n", raw_status,
           bladerf_strerror(raw_status), BLADERF_ERR_UNSUPPORTED);
    if (raw_status != BLADERF_ERR_UNSUPPORTED) goto fail;
    raw_status = bladerf_sync_config(dev, rx_layout,
                                     BLADERF_FORMAT_SC16_Q11, 8, 4096, 4, 3000);
    if (raw_status != BLADERF_ERR_UNSUPPORTED) goto fail;
    s = bladerf_rf_events_get_since(dev, event_cursor, events,
                                    BLADERF_RF_EVENT_HISTORY_SIZE,
                                    &event_count, &next_event_cursor,
                                    &history_complete);
    if (s != 0 || !history_complete) {
        fprintf(stderr, "format event query failed: %s\n", bladerf_strerror(s));
        goto fail;
    }
    for (uint32_t i = 0; i < event_count; ++i) {
        if (events[i].event_type == BLADERF_RF_EVT_RX_FORMAT_UNSUPPORTED &&
            events[i].flags == BLADERF_FORMAT_SC16_Q11 &&
            events[i].error_code == BLADERF_ERR_UNSUPPORTED) {
            format_event_count++;
        }
    }
    if (format_event_count != 2) {
        fprintf(stderr, "expected 2 RX_FORMAT_UNSUPPORTED events, found %u\n",
                format_event_count);
        goto fail;
    }
    s = bladerf_sync_config(dev, rx_layout,
                            BLADERF_FORMAT_SC16_Q11_META, 8, 4096, 4, 3000);
    if (s != 0) { fprintf(stderr, "META sync config: %s\n", bladerf_strerror(s)); goto fail; }
    s = bladerf_sync_rx(dev, samples, 4096, &meta, 1000);
    printf("pre-transition sync RX status=%d (%s), actual_count=%u\n", s,
           bladerf_strerror(s), meta.actual_count);
    if (s != BLADERF_ERR_WOULD_BLOCK) goto fail;
    s = bladerf_rf_events_get_since(dev, next_event_cursor, events,
                                    BLADERF_RF_EVENT_HISTORY_SIZE,
                                    &event_count, &event_cursor,
                                    &history_complete);
    if (s != 0 || !history_complete) {
        fprintf(stderr, "withheld event query failed: %s\n", bladerf_strerror(s));
        goto fail;
    }
    bool withheld_event_found = false;
    for (uint32_t i = 0; i < event_count; ++i) {
        if (events[i].event_type == BLADERF_RF_EVT_RX_DATA_WITHHELD &&
            (events[i].flags & ~BLADERF_RF_EVENT_F_FPGA_TIMESTAMP_VALID) ==
                BLADERF_RF_WITHHELD_EPOCH_UNCERTIFIED) {
            withheld_event_found = true;
            break;
        }
    }
    if (!withheld_event_found) {
        fprintf(stderr, "invalid sync RX read did not publish RX_DATA_WITHHELD\n");
        goto fail;
    }
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
    bladerf_enable_module(dev, rx_channel, false);
    bladerf_close(dev);
    return 0;
fail:
    if (dev != NULL) bladerf_close(dev);
    return 2;
}
