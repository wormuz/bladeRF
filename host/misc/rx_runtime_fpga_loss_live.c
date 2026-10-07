#define _DEFAULT_SOURCE

/* Test-build xA4 qualification for the FPGA RX loss-counter notification.
 * The monitor's test injection increments the observed counter without
 * disturbing the RFIC or USB link. */
#include <libbladeRF.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RX_SAMPLES 4096

int main(int argc, char **argv)
{
    struct bladerf *dev = NULL;
    bladerf_channel transition_channel = BLADERF_CHANNEL_RX(0);
    bladerf_channel_layout layout = BLADERF_RX_X1;
    bool enable_rx1 = true;
    bool enable_rx2 = false;
    const char *layout_name = "RX1";
    int16_t samples[RX_SAMPLES * 4];
    int status;

    if (argc > 2 || (argc == 2 && strcmp(argv[1], "RX1") != 0 &&
                     strcmp(argv[1], "RX2") != 0 &&
                     strcmp(argv[1], "RX_X2") != 0)) {
        fprintf(stderr, "usage: %s [RX1|RX2|RX_X2]\n", argv[0]);
        return 2;
    }
    if (argc == 2 && strcmp(argv[1], "RX2") == 0) {
        transition_channel = BLADERF_CHANNEL_RX(1);
        enable_rx1 = false;
        enable_rx2 = true;
        layout_name = "RX2";
    } else if (argc == 2 && strcmp(argv[1], "RX_X2") == 0) {
        transition_channel = BLADERF_CHANNEL_RX(1);
        enable_rx2 = true;
        layout = BLADERF_RX_X2;
        layout_name = "RX_X2";
    }

    status = bladerf_open(&dev, NULL);
    if (status != 0) goto cleanup;
    bladerf_log_set_verbosity(BLADERF_LOG_LEVEL_WARNING);
    status = bladerf_set_sample_rate(dev, transition_channel, 1000000, NULL);
    if (status != 0) goto cleanup;
    status = bladerf_set_bandwidth(dev, transition_channel, 1500000, NULL);
    if (status != 0) goto cleanup;
    status = bladerf_sync_config(dev, layout, BLADERF_FORMAT_SC16_Q11_META,
                                 16, 8192, 8, 1000);
    if (status != 0) goto cleanup;
    if (enable_rx1) {
        status = bladerf_enable_module(dev, BLADERF_CHANNEL_RX(0), true);
        if (status != 0) goto cleanup;
    }
    if (enable_rx2) {
        status = bladerf_enable_module(dev, BLADERF_CHANNEL_RX(1), true);
        if (status != 0) goto cleanup;
    }

    struct bladerf_rx_transition_request request = {
        .target_frequency_hz = 1835000000ULL,
        .required_events_mask = BLADERF_RF_REQUIRE_EPOCH_VALID,
        .require_rx_data_valid = true,
        .timeout_ms = 2000,
    };
    uint32_t transaction_id = 0;
    struct bladerf_rf_event epoch_event = {0};
    status = bladerf_rx_transition_begin(dev, transition_channel, &request,
                                         &transaction_id);
    if (status != 0) goto cleanup;
    status = bladerf_rx_transition_wait(dev, transaction_id, &epoch_event,
                                        2000);
    if (status != 0 ||
        epoch_event.event_type != BLADERF_RF_EVT_RX_EPOCH_VALID) {
        fprintf(stderr, "initial epoch failed: %s event=%u\n",
                bladerf_strerror(status), epoch_event.event_type);
        status = BLADERF_ERR_UNEXPECTED;
        goto cleanup;
    }

    struct bladerf_rf_event events[BLADERF_RF_EVENT_HISTORY_SIZE];
    uint32_t event_count = 0;
    uint64_t cursor = 0;
    bool history_complete = false;
    status = bladerf_rf_events_get_since(
        dev, 0, events, BLADERF_RF_EVENT_HISTORY_SIZE, &event_count,
        &cursor, &history_complete);
    if (status != 0 || !history_complete) {
        status = status != 0 ? status : BLADERF_ERR_UNEXPECTED;
        goto cleanup;
    }

    if (setenv("BLADERF_TEST_RX_TRANSITION_STALL",
               "RUNTIME_FPGA_RX_LOSS", 1) != 0) {
        status = BLADERF_ERR_UNEXPECTED;
        goto cleanup;
    }
    bool saw_fpga_loss = false;
    for (unsigned attempt = 0; attempt < 3000 && !saw_fpga_loss; ++attempt) {
        usleep(1000);
        uint64_t next_cursor = cursor;
        status = bladerf_rf_events_get_since(
            dev, cursor, events, BLADERF_RF_EVENT_HISTORY_SIZE,
            &event_count, &next_cursor, &history_complete);
        if (status != 0 || !history_complete) {
            status = status != 0 ? status : BLADERF_ERR_UNEXPECTED;
            goto cleanup;
        }
        cursor = next_cursor;
        for (uint32_t i = 0; i < event_count; ++i) {
            if (events[i].event_type == BLADERF_RF_EVT_RX_STREAM_OVERRUN &&
                (events[i].flags &
                 BLADERF_RF_STREAM_STATUS_FPGA_RX_LOSS) != 0 &&
                events[i].epoch_id == epoch_event.epoch_id) {
                epoch_event.rfic_status = events[i].rfic_status;
                saw_fpga_loss = true;
                break;
            }
        }
    }
    unsetenv("BLADERF_TEST_RX_TRANSITION_STALL");
    if (!saw_fpga_loss) {
        fprintf(stderr, "FPGA RX loss event not observed\n");
        status = BLADERF_ERR_UNEXPECTED;
        goto cleanup;
    }

    const unsigned int expected_samples =
        layout == BLADERF_RX_X2 ? RX_SAMPLES * 2 : RX_SAMPLES;
    struct bladerf_metadata metadata = {0};
    bool saw_sync_overrun = false;
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        memset(&metadata, 0, sizeof(metadata));
        metadata.flags = BLADERF_META_FLAG_RX_NOW;
        status = bladerf_sync_rx(dev, samples, expected_samples,
                                 &metadata, 100);
        saw_sync_overrun |=
            (metadata.status & BLADERF_META_STATUS_OVERRUN) != 0;
        if (status == 0 && metadata.actual_count == expected_samples) {
            break;
        }
        if (status != BLADERF_ERR_WOULD_BLOCK) {
            break;
        }
        usleep(10000);
    }
    if (status != 0 || metadata.actual_count != expected_samples ||
        !metadata.rx_epoch_id_valid ||
        metadata.rx_epoch_id != epoch_event.epoch_id ||
        !saw_sync_overrun) {
        fprintf(stderr, "FPGA loss not carried to sync read: %s count=%u "
                "status=0x%x epoch=%u/%u valid=%u\n",
                bladerf_strerror(status), metadata.actual_count,
                metadata.status, metadata.rx_epoch_id, epoch_event.epoch_id,
                metadata.rx_epoch_id_valid);
        status = BLADERF_ERR_UNEXPECTED;
        goto cleanup;
    }

    printf("FPGA RX loss event: PASS layout=%s epoch=%u counter_low=%u "
           "sync_overrun=1 IQ_valid=1\n", layout_name, epoch_event.epoch_id,
           epoch_event.rfic_status);
    status = 0;

cleanup:
    unsetenv("BLADERF_TEST_RX_TRANSITION_STALL");
    if (dev != NULL) {
        if (enable_rx1) {
            (void)bladerf_enable_module(dev, BLADERF_CHANNEL_RX(0), false);
        }
        if (enable_rx2) {
            (void)bladerf_enable_module(dev, BLADERF_CHANNEL_RX(1), false);
        }
        bladerf_close(dev);
    }
    if (status != 0) {
        fprintf(stderr, "FPGA RX loss test failed: %s\n",
                bladerf_strerror(status));
    }
    return status == 0 ? 0 : 1;
}
