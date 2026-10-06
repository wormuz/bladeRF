/* Live xA4 fault-injection check for fail-closed RX epoch ARM handling.
 * Build libbladeRF with ENABLE_TEST_RX_ABORT_FAULT_INJECTION=ON, then run
 * BLADERF_TEST_FAIL_RX_EPOCH_ARM=1 ./rx_epoch_arm_failure_live. */
#include <libbladeRF.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    const bladerf_frequency initial_hz = 1.835e9;
    const bladerf_frequency requested_hz = 1.8354e9;
    struct bladerf *dev = NULL;
    struct bladerf_rf_event events[BLADERF_RF_EVENT_HISTORY_SIZE];
    uint32_t transaction_id = 0;
    uint32_t event_count = 0;
    bool history_complete = false;
    bladerf_frequency readback_hz = 0;
    int status = bladerf_open(&dev, NULL);
    if (status != 0) {
        fprintf(stderr, "bladerf_open: %s\n", bladerf_strerror(status));
        return 1;
    }

    status = bladerf_set_frequency(dev, BLADERF_CHANNEL_RX(0), initial_hz);
    if (status != 0) {
        fprintf(stderr, "set initial LO: %s\n", bladerf_strerror(status));
        goto fail;
    }

    if (setenv("BLADERF_TEST_FAIL_RX_EPOCH_ARM", "1", 1) != 0) {
        perror("setenv");
        goto fail;
    }

    const struct bladerf_rx_transition_request request = {
        .target_frequency_hz = requested_hz,
        .required_events_mask = BLADERF_RF_REQUIRE_EPOCH_VALID |
                                BLADERF_RF_REQUIRE_PLL_LOCKED,
        .timeout_ms = 1000,
        .require_rx_data_valid = true,
        .epoch_settle_samples = 0,
    };
    status = bladerf_rx_transition_begin(dev, BLADERF_CHANNEL_RX(0),
                                          &request, &transaction_id);
    unsetenv("BLADERF_TEST_FAIL_RX_EPOCH_ARM");
    if (status == 0) {
        fprintf(stderr, "injected ARM failure unexpectedly succeeded\n");
        goto fail;
    }

    int query_status = bladerf_rx_transition_get_events(
        dev, transaction_id, events, BLADERF_RF_EVENT_HISTORY_SIZE,
        &event_count, &history_complete);
    if (query_status != 0 || !history_complete) {
        fprintf(stderr, "event history unavailable: %s complete=%d\n",
                bladerf_strerror(query_status), history_complete);
        goto fail;
    }

    bool saw_config = false;
    bool saw_error = false;
    bool saw_valid = false;
    for (uint32_t i = 0; i < event_count; ++i) {
        saw_config |= events[i].event_type == BLADERF_RF_EVT_CONFIG_ACCEPTED;
        saw_error |= events[i].event_type == BLADERF_RF_EVT_ERROR;
        saw_valid |= events[i].event_type == BLADERF_RF_EVT_RX_EPOCH_VALID ||
                     events[i].event_type ==
                         BLADERF_RF_EVT_RX_FIRST_VALID_HOST_DATA;
    }

    query_status = bladerf_get_frequency(dev, BLADERF_CHANNEL_RX(0),
                                          &readback_hz);
    if (query_status != 0 || !saw_config || !saw_error || saw_valid ||
        readback_hz < initial_hz - 100 || readback_hz > initial_hz + 100) {
        fprintf(stderr,
                "FAIL txn=%u events=%u config=%d error=%d valid=%d "
                "LO=%llu status=%d\n",
                transaction_id, event_count, saw_config, saw_error, saw_valid,
                (unsigned long long)readback_hz, query_status);
        goto fail;
    }

    printf("PASS txn=%u begin_status=%s events=%u config=%d error=%d "
           "valid=%d LO=%llu Hz\n",
           transaction_id, bladerf_strerror(status), event_count,
           saw_config, saw_error, saw_valid,
           (unsigned long long)readback_hz);
    bladerf_close(dev);
    return 0;

fail:
    unsetenv("BLADERF_TEST_FAIL_RX_EPOCH_ARM");
    bladerf_close(dev);
    return 1;
}
