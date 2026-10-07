#define _DEFAULT_SOURCE

/* Test-build xA4 check: the board control-plane monitor must notice a sticky
 * FPGA RX fault after epoch certification, revoke it, and publish the source
 * status without waiting for a libusb completion callback. Build with
 * ENABLE_TEST_RX_TRANSITION_STALL_INJECTION=ON and run with
 * BLADERF_TEST_RX_TRANSITION_STALL=RUNTIME_FPGA_FAULT, or use the
 * RUNTIME_FPGA_STATUS_READ_FAILURE / RUNTIME_FPGA_STATUS_VERSION values to
 * verify fail-closed behavior when the monitor cannot trust the status. */
#include <libbladeRF.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RX_FAULT_STATUS_BIT (1u << 14)

int main(int argc, char **argv)
{
    struct bladerf *dev = NULL;
    int16_t samples[8192 * 4];
    bladerf_channel transition_channel = BLADERF_CHANNEL_RX(0);
    bladerf_channel_layout layout = BLADERF_RX_X1;
    bool enable_rx1 = true;
    bool enable_rx2 = false;
    const char *fault_mode = getenv("BLADERF_TEST_RX_TRANSITION_STALL");
    uint32_t expected_reason = BLADERF_RF_INVALIDATE_FPGA_RX_FAULT;
    int expected_error = 0;
    uint32_t transaction_id = 0;
    int status;

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
        return 2;
    }
    if (fault_mode != NULL &&
        strcmp(fault_mode, "RUNTIME_FPGA_FAULT") != 0) {
        expected_reason = BLADERF_RF_INVALIDATE_FPGA_STATUS_UNAVAILABLE;
        expected_error = strcmp(fault_mode,
            "RUNTIME_FPGA_STATUS_READ_FAILURE") == 0 ? BLADERF_ERR_IO :
            BLADERF_ERR_UNEXPECTED;
    }

    status = bladerf_open(&dev, NULL);
    if (status != 0) goto cleanup;
    bladerf_log_set_verbosity(BLADERF_LOG_LEVEL_WARNING);

    status = bladerf_set_sample_rate(dev, transition_channel, 4000000, NULL);
    if (status != 0) goto cleanup;
    status = bladerf_set_bandwidth(dev, transition_channel, 5000000, NULL);
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
        .required_events_mask = BLADERF_RF_REQUIRE_PLL_LOCKED,
        .timeout_ms = 2000,
        .require_rx_data_valid = true,
        .epoch_settle_samples = 0,
    };
    struct bladerf_rf_event transition_event = {0};
    status = bladerf_rx_transition_begin(dev, transition_channel, &request,
                                         &transaction_id);
    if (status != 0) goto cleanup;
    status = bladerf_rx_transition_wait(dev, transaction_id,
                                        &transition_event, 2000);
    if (status != 0 ||
        transition_event.event_type != BLADERF_RF_EVT_RX_EPOCH_VALID) {
        fprintf(stderr, "pre-fault transition failed: %s event=%u\n",
                bladerf_strerror(status), transition_event.event_type);
        status = BLADERF_ERR_UNEXPECTED;
        goto cleanup;
    }

    struct bladerf_rf_event events[BLADERF_RF_EVENT_HISTORY_SIZE];
    uint32_t event_count = 0;
    uint64_t cursor = 0;
    bool complete = false;
    status = bladerf_rf_events_get_since(
        dev, 0, events, BLADERF_RF_EVENT_HISTORY_SIZE, &event_count,
        &cursor, &complete);
    if (status != 0 || !complete) {
        status = status != 0 ? status : BLADERF_ERR_UNEXPECTED;
        goto cleanup;
    }

    bool saw_fault_invalidation = false;
    for (unsigned attempt = 0; attempt < 40 && !saw_fault_invalidation;
         ++attempt) {
        usleep(50000);
        uint64_t next_cursor = cursor;
        status = bladerf_rf_events_get_since(
            dev, cursor, events, BLADERF_RF_EVENT_HISTORY_SIZE,
            &event_count, &next_cursor, &complete);
        if (status != 0 || !complete) {
            status = status != 0 ? status : BLADERF_ERR_UNEXPECTED;
            goto cleanup;
        }
        cursor = next_cursor;
        for (uint32_t i = 0; i < event_count; ++i) {
            if (events[i].event_type == BLADERF_RF_EVT_RX_DATA_INVALIDATED &&
                events[i].flags == expected_reason &&
                (expected_reason != BLADERF_RF_INVALIDATE_FPGA_RX_FAULT ||
                 (events[i].rfic_status & RX_FAULT_STATUS_BIT) != 0) &&
                (expected_error == 0 || events[i].error_code == expected_error) &&
                events[i].epoch_id == transition_event.epoch_id) {
                saw_fault_invalidation = true;
                break;
            }
        }
    }
    if (!saw_fault_invalidation) {
        fprintf(stderr, "runtime FPGA fault invalidation event not observed\n");
        status = BLADERF_ERR_UNEXPECTED;
        goto cleanup;
    }

    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i) {
        samples[i] = 0x5555;
    }
    struct bladerf_metadata metadata = { .flags = BLADERF_META_FLAG_RX_NOW };
    int rx_status = bladerf_sync_rx(dev, samples, 8192, &metadata, 100);
    if (rx_status == 0 || metadata.actual_count != 0) {
        fprintf(stderr, "runtime FPGA fault leaked IQ: status=%s count=%u\n",
                bladerf_strerror(rx_status), metadata.actual_count);
        status = BLADERF_ERR_UNEXPECTED;
        goto cleanup;
    }
    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i) {
        if (samples[i] != 0x5555) {
            fprintf(stderr, "runtime FPGA fault modified sample buffer\n");
            status = BLADERF_ERR_UNEXPECTED;
            goto cleanup;
        }
    }

    printf("runtime FPGA RX fault monitor: PASS layout=%s epoch=%u "
           "reason=0x%x iq_count=0\n",
           layout == BLADERF_RX_X2 ? "RX_X2" :
           (transition_channel == BLADERF_CHANNEL_RX(1) ? "RX2" : "RX1"),
           transition_event.epoch_id, expected_reason);
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
        fprintf(stderr, "runtime FPGA RX fault test failed: %s\n",
                bladerf_strerror(status));
    }
    return status == 0 ? 0 : 1;
}
