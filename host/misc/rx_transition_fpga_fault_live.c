/* Test-build xA4 check: a sticky FPGA RX fault must prevent epoch
 * certification and keep sync IQ fenced. Build the library with
 * ENABLE_TEST_RX_TRANSITION_STALL_INJECTION=ON and run with
 * BLADERF_TEST_RX_TRANSITION_STALL=FPGA_FAULT. */
#include <libbladeRF.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RX_FAULT_STATUS_BIT (1u << 14)

int main(int argc, char **argv)
{
    struct bladerf *dev = NULL;
    int16_t samples[8192 * 4];
    bladerf_channel transition_channel = BLADERF_CHANNEL_RX(0);
    bladerf_channel_layout layout = BLADERF_RX_X1;
    bool enable_rx1 = true;
    bool enable_rx2 = false;
    uint32_t transaction_id = 0;
    uint32_t event_count = 0;
    bool event_history_complete = false;
    bool saw_fault_error = false;
    bool saw_epoch_valid = false;
    int status;

    status = bladerf_open(&dev, NULL);
    if (status != 0) {
        goto cleanup;
    }
    bladerf_log_set_verbosity(BLADERF_LOG_LEVEL_WARNING);

    if (argc > 1 && strcmp(argv[1], "RX2") == 0) {
        transition_channel = BLADERF_CHANNEL_RX(1);
        enable_rx1 = false;
        enable_rx2 = true;
    } else if (argc > 1 && strcmp(argv[1], "RX_X2") == 0) {
        transition_channel = BLADERF_CHANNEL_RX(1);
        layout = BLADERF_RX_X2;
        enable_rx2 = true;
    } else if (argc > 1 && strcmp(argv[1], "RX1") != 0) {
        fprintf(stderr, "usage: %s [RX1|RX2|RX_X2]\n", argv[0]);
        status = BLADERF_ERR_INVAL;
        goto cleanup;
    }

    status = bladerf_set_sample_rate(dev, transition_channel, 4000000, NULL);
    if (status != 0) goto cleanup;
    status = bladerf_set_bandwidth(dev, transition_channel, 5000000, NULL);
    if (status != 0) goto cleanup;
    status = bladerf_sync_config(dev, layout,
                                 BLADERF_FORMAT_SC16_Q11_META,
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

    const struct bladerf_rx_transition_request request = {
        .target_frequency_hz = 1835000000ULL,
        .required_events_mask = BLADERF_RF_REQUIRE_PLL_LOCKED,
        .timeout_ms = 2000,
        .require_rx_data_valid = true,
        .epoch_settle_samples = 0,
    };
    status = bladerf_rx_transition_begin(dev, transition_channel,
                                         &request, &transaction_id);
    if (status != 0) goto cleanup;

    struct bladerf_rf_event final_event = {0};
    status = bladerf_rx_transition_wait(dev, transaction_id, &final_event,
                                        2000);
    if (status != BLADERF_ERR_UNEXPECTED) {
        fprintf(stderr, "faulted transition returned %s, expected FPGA fault\n",
                bladerf_strerror(status));
        status = BLADERF_ERR_UNEXPECTED;
        goto cleanup;
    }

    struct bladerf_rf_event events[BLADERF_RF_EVENT_HISTORY_SIZE];
    status = bladerf_rx_transition_get_events(
        dev, transaction_id, events, BLADERF_RF_EVENT_HISTORY_SIZE,
        &event_count, &event_history_complete);
    if (status != 0 || !event_history_complete) {
        status = status != 0 ? status : BLADERF_ERR_UNEXPECTED;
        goto cleanup;
    }
    for (uint32_t i = 0; i < event_count; ++i) {
        if (events[i].event_type == BLADERF_RF_EVT_RX_EPOCH_VALID) {
            saw_epoch_valid = true;
        }
        if (events[i].event_type == BLADERF_RF_EVT_ERROR &&
            events[i].error_code == BLADERF_ERR_UNEXPECTED &&
            (events[i].rfic_status & RX_FAULT_STATUS_BIT) != 0) {
            saw_fault_error = true;
        }
    }
    if (!saw_fault_error || saw_epoch_valid) {
        fprintf(stderr, "missing FPGA fault error or false epoch-valid event\n");
        status = BLADERF_ERR_UNEXPECTED;
        goto cleanup;
    }

    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i) {
        samples[i] = 0x5555;
    }
    struct bladerf_metadata metadata = { .flags = BLADERF_META_FLAG_RX_NOW };
    int rx_status = bladerf_sync_rx(dev, samples, 8192, &metadata, 50);
    if (rx_status == 0 || metadata.actual_count != 0) {
        fprintf(stderr, "faulted transition leaked IQ: status=%s count=%u\n",
                bladerf_strerror(rx_status), metadata.actual_count);
        status = BLADERF_ERR_UNEXPECTED;
        goto cleanup;
    }
    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i) {
        if (samples[i] != 0x5555) {
            fprintf(stderr, "faulted transition modified sample buffer\n");
            status = BLADERF_ERR_UNEXPECTED;
            goto cleanup;
        }
    }

    printf("FPGA RX fault fence: PASS layout=%s txn=%u fault_error=1 "
           "epoch_valid=0 iq_count=0\n",
           layout == BLADERF_RX_X2 ? "RX_X2" :
           (transition_channel == BLADERF_CHANNEL_RX(1) ? "RX2" : "RX1"),
           transaction_id);
    status = 0;

cleanup:
    if (dev != NULL) {
        if (enable_rx1) {
            bladerf_enable_module(dev, BLADERF_CHANNEL_RX(0), false);
        }
        if (enable_rx2) {
            bladerf_enable_module(dev, BLADERF_CHANNEL_RX(1), false);
        }
        bladerf_close(dev);
    }
    if (status != 0) {
        fprintf(stderr, "FPGA RX fault test failed: %s\n",
                bladerf_strerror(status));
    }
    return status == 0 ? 0 : 1;
}
