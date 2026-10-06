/* Hardware failure-path qualification for ADR-0207.
 *
 * Exercise a failure after the FPGA has fenced the old epoch: a valid
 * transition first establishes data, then an out-of-range LO request must
 * fail and ABORT the pending epoch. The host META parser must not return
 * queued samples from the previous epoch. A later explicit transition must
 * recover the ERROR gate and return only the newly admitted epoch.
 *
 * This does not write FPGA flash or inject an RFIC/USB electrical fault.
 */
#include <libbladeRF.h>

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static int transition(struct bladerf *dev, uint64_t frequency_hz,
                      struct bladerf_rf_event *result)
{
    const struct bladerf_rx_transition_request request = {
        .target_frequency_hz = frequency_hz,
        .required_events_mask = BLADERF_RF_REQUIRE_PLL_LOCKED |
                                BLADERF_RF_REQUIRE_ENSM_RX |
                                BLADERF_RF_REQUIRE_EPOCH_VALID,
        .timeout_ms = 2000,
        .require_rx_data_valid = true,
        .epoch_settle_samples = 0,
    };
    uint32_t transaction_id = 0;
    int status = bladerf_rx_transition_begin(
        dev, BLADERF_CHANNEL_RX(0), &request, &transaction_id);
    if (status != 0) {
        fprintf(stderr, "transition begin %.3f MHz: %s txn=%u\n",
                frequency_hz / 1e6, bladerf_strerror(status), transaction_id);
        return status;
    }
    status = bladerf_rx_transition_wait(dev, transaction_id, result, 2500);
    if (status != 0) {
        fprintf(stderr, "transition wait %.3f MHz: %s txn=%u\n",
                frequency_hz / 1e6, bladerf_strerror(status), transaction_id);
    }
    return status;
}

static int read_valid_epoch(struct bladerf *dev, int16_t *samples,
                            uint8_t expected_epoch, uint64_t min_timestamp)
{
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        struct bladerf_metadata metadata = {0};
        metadata.flags = BLADERF_META_FLAG_RX_NOW;
        int status = bladerf_sync_rx(dev, samples, 4096, &metadata, 2000);
        if (status != 0) {
            fprintf(stderr, "sync_rx: %s\n", bladerf_strerror(status));
            return status;
        }
        if (!metadata.rx_epoch_id_valid ||
            metadata.rx_epoch_id != expected_epoch ||
            metadata.actual_count == 0 ||
            metadata.timestamp < min_timestamp) {
            fprintf(stderr, "invalid IQ: epoch=%u expected=%u valid=%u count=%u "
                    "status=0x%x timestamp=%" PRIu64 " boundary=%" PRIu64 "\n",
                    metadata.rx_epoch_id, expected_epoch,
                    metadata.rx_epoch_id_valid, metadata.actual_count,
                    metadata.status, metadata.timestamp, min_timestamp);
            return BLADERF_ERR_UNEXPECTED;
        }
        if (metadata.actual_count == 4096 &&
            (metadata.status & BLADERF_META_STATUS_OVERRUN) == 0) {
            return 0;
        }
        fprintf(stderr, "PARTIAL_VALID_READ attempt=%u epoch=%u count=%u "
                "status=0x%x timestamp=%" PRIu64 "\n", attempt + 1,
                metadata.rx_epoch_id, metadata.actual_count,
                metadata.status, metadata.timestamp);
    }
    return BLADERF_ERR_UNEXPECTED;
}

static int check_failed_trace(struct bladerf *dev, uint32_t transaction_id)
{
    struct bladerf_rf_event events[BLADERF_RF_EVENT_HISTORY_SIZE];
    uint32_t count = 0;
    bool complete = false;
    int status = bladerf_rx_transition_get_events(
        dev, transaction_id, events, BLADERF_RF_EVENT_HISTORY_SIZE,
        &count, &complete);
    if (status != 0 || !complete || count == 0) {
        fprintf(stderr, "failed transition trace unavailable txn=%u status=%s "
                "complete=%u count=%u\n", transaction_id,
                bladerf_strerror(status), complete, count);
        return status ? status : BLADERF_ERR_UNEXPECTED;
    }

    bool invalidated = false;
    bool errored = false;
    bool valid = false;
    for (uint32_t i = 0; i < count; ++i) {
        if (events[i].transaction_id != transaction_id) {
            return BLADERF_ERR_UNEXPECTED;
        }
        invalidated |= events[i].event_type == BLADERF_RF_EVT_RX_EPOCH_INVALID;
        errored |= events[i].event_type == BLADERF_RF_EVT_ERROR;
        valid |= events[i].event_type == BLADERF_RF_EVT_RX_EPOCH_VALID;
    }
    if (!invalidated || !errored || valid) {
        fprintf(stderr, "failed transition trace invalid txn=%u "
                "invalidated=%u error=%u epoch_valid=%u events=%u\n",
                transaction_id, invalidated, errored, valid, count);
        return BLADERF_ERR_UNEXPECTED;
    }
    return 0;
}

int main(void)
{
    const uint64_t recovery_frequency_hz = 1835400000ULL;
    const uint64_t invalid_frequency_hz = 7000000000ULL;
    int16_t *samples = calloc(4096 * 2, sizeof(*samples));
    struct bladerf *dev = NULL;
    struct bladerf_rf_event initial = {0};
    struct bladerf_rf_event recovered = {0};
    int status = BLADERF_ERR_UNEXPECTED;
    if (samples == NULL) {
        return 2;
    }

    status = bladerf_open(&dev, NULL);
    if (status != 0) {
        fprintf(stderr, "open: %s\n", bladerf_strerror(status));
        goto out;
    }
    bladerf_log_set_verbosity(BLADERF_LOG_LEVEL_WARNING);
#define CHECK(call) do { \
    status = (call); \
    if (status != 0) { \
        fprintf(stderr, "%s: %s\n", #call, bladerf_strerror(status)); \
        goto out; \
    } \
} while (0)

    CHECK(bladerf_set_sample_rate(dev, BLADERF_CHANNEL_RX(0), 4000000, NULL));
    CHECK(bladerf_set_bandwidth(dev, BLADERF_CHANNEL_RX(0), 5000000, NULL));
    CHECK(bladerf_set_gain(dev, BLADERF_CHANNEL_RX(0), 30));
    CHECK(bladerf_sync_config(dev, BLADERF_RX_X1,
                              BLADERF_FORMAT_SC16_Q11_META,
                              16, 8192, 8, 1000));
    CHECK(bladerf_enable_module(dev, BLADERF_CHANNEL_RX(0), true));

    CHECK(transition(dev, recovery_frequency_hz, &initial));
    CHECK(read_valid_epoch(dev, samples, initial.epoch_id,
                           initial.fpga_timestamp));

    struct bladerf_range const *range = NULL;
    CHECK(bladerf_get_frequency_range(dev, BLADERF_CHANNEL_RX(0), &range));
    if (range == NULL || invalid_frequency_hz <= (uint64_t)range->max) {
        fprintf(stderr, "invalid test frequency is not outside RX range\n");
        status = BLADERF_ERR_UNEXPECTED;
        goto out;
    }

    const struct bladerf_rx_transition_request invalid_request = {
        .target_frequency_hz = invalid_frequency_hz,
        .required_events_mask = BLADERF_RF_REQUIRE_PLL_LOCKED |
                                BLADERF_RF_REQUIRE_ENSM_RX |
                                BLADERF_RF_REQUIRE_EPOCH_VALID,
        .timeout_ms = 2000,
        .require_rx_data_valid = true,
        .epoch_settle_samples = 0,
    };
    uint32_t failed_transaction_id = 0;
    status = bladerf_rx_transition_begin(
        dev, BLADERF_CHANNEL_RX(0), &invalid_request,
        &failed_transaction_id);
    if (status != BLADERF_ERR_RANGE) {
        fprintf(stderr, "out-of-range transition returned %s, expected %s\n",
                bladerf_strerror(status), bladerf_strerror(BLADERF_ERR_RANGE));
        status = BLADERF_ERR_UNEXPECTED;
        goto out;
    }
    status = check_failed_trace(dev, failed_transaction_id);
    if (status != 0) {
        goto out;
    }

    /* All old-epoch packets must be consumed and dropped by the pending-ID
     * filter. No epoch-valid samples can arrive while the FPGA gate is in
     * ERROR, so the synchronous read must end only with a timeout. */
    struct bladerf_metadata metadata = {0};
    metadata.flags = BLADERF_META_FLAG_RX_NOW;
    status = bladerf_sync_rx(dev, samples, 4096, &metadata, 300);
    if (status != BLADERF_ERR_TIMEOUT) {
        fprintf(stderr, "failed epoch read returned %s (epoch=%u valid=%u "
                "count=%u), expected timeout\n", bladerf_strerror(status),
                metadata.rx_epoch_id, metadata.rx_epoch_id_valid,
                metadata.actual_count);
        status = BLADERF_ERR_UNEXPECTED;
        goto out;
    }

    CHECK(transition(dev, recovery_frequency_hz, &recovered));
    if (recovered.epoch_id == initial.epoch_id) {
        fprintf(stderr, "recovery reused epoch ID %u\n", recovered.epoch_id);
        status = BLADERF_ERR_UNEXPECTED;
        goto out;
    }
    CHECK(read_valid_epoch(dev, samples, recovered.epoch_id,
                           recovered.fpga_timestamp));

    printf("RX epoch ABORT qualification: PASS initial_epoch=%u "
           "failed_txn=%u recovered_epoch=%u; stale IQ rejected, "
           "explicit ARM recovery valid\n", initial.epoch_id,
           failed_transaction_id, recovered.epoch_id);
    status = 0;

out:
    if (dev != NULL) {
        (void)bladerf_enable_module(dev, BLADERF_CHANNEL_RX(0), false);
        bladerf_close(dev);
    }
    free(samples);
    return status == 0 ? 0 : 1;
}
