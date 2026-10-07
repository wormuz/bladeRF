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
#include <string.h>

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
    } else {
        fprintf(stderr, "transition complete txn=%u event=%u state=%u epoch=%u "
                "requested=%" PRIu64 " readback=%" PRIu64
                " first_ts=%" PRIu64 " error=%d\n",
                result->transaction_id, result->event_type,
                result->fpga_state, result->epoch_id,
                result->requested_rx_lo_hz, result->readback_rx_lo_hz,
                result->fpga_timestamp, result->error_code);
    }
    return status;
}

static void dump_recent_rx_events(struct bladerf *dev)
{
    struct bladerf_rf_event events[BLADERF_RF_EVENT_HISTORY_SIZE];
    uint32_t count = 0;
    uint64_t next_sequence = 0;
    bool complete = false;
    int status = bladerf_rf_events_get_since(
        dev, 0, events, BLADERF_RF_EVENT_HISTORY_SIZE, &count,
        &next_sequence, &complete);

    fprintf(stderr, "recent RF events: status=%s count=%u next=%" PRIu64
            " complete=%u\n", bladerf_strerror(status), count,
            next_sequence, complete);
    if (status != 0) {
        return;
    }
    for (uint32_t i = 0; i < count; ++i) {
        fprintf(stderr, "  event[%u] type=%u txn=%u epoch=%u flags=0x%x "
                "state=%u ts=%" PRIu64 " error=%d\n", i,
                events[i].event_type, events[i].transaction_id,
                events[i].epoch_id, events[i].flags, events[i].fpga_state,
                events[i].fpga_timestamp, events[i].error_code);
    }
}

static int read_valid_epoch(struct bladerf *dev, int16_t *samples,
                            uint8_t expected_epoch, uint64_t min_timestamp)
{
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        struct bladerf_metadata metadata = {0};
        metadata.flags = BLADERF_META_FLAG_RX_NOW;
        int status = bladerf_sync_rx(dev, samples, 4096, &metadata, 2000);
        if (status != 0) {
            if (status == BLADERF_ERR_WOULD_BLOCK) {
                fprintf(stderr, "RX data withheld; retrying explicit read\n");
                continue;
            }
            fprintf(stderr, "sync_rx: %s\n", bladerf_strerror(status));
            fprintf(stderr, "read_valid_epoch: expected_epoch=%u boundary=%"
                    PRIu64 " actual_count=%u meta_epoch=%u epoch_valid=%u "
                    "metadata_timestamp=%" PRIu64 " metadata_status=0x%x\n",
                    expected_epoch, min_timestamp, metadata.actual_count,
                    metadata.rx_epoch_id, metadata.rx_epoch_id_valid,
                    metadata.timestamp, metadata.status);
            bladerf_timestamp current_rx_timestamp = 0;
            int timestamp_status = bladerf_get_timestamp(
                dev, BLADERF_RX, &current_rx_timestamp);
            fprintf(stderr, "device RX timestamp after failed read: %s "
                    "value=%" PRIu64 "\n",
                    bladerf_strerror(timestamp_status), current_rx_timestamp);
            dump_recent_rx_events(dev);
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
    bool spi_started = false;
    bool spi_done = false;
    bool lo_readback = false;
    bool pll_locked = false;
    bool ensm_rx = false;
    uint64_t previous_timestamp = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (events[i].transaction_id != transaction_id) {
            return BLADERF_ERR_UNEXPECTED;
        }
        if (events[i].host_monotonic_ns < previous_timestamp) {
            fprintf(stderr, "failed transition events out of timestamp order\n");
            return BLADERF_ERR_UNEXPECTED;
        }
        previous_timestamp = events[i].host_monotonic_ns;
        invalidated |= events[i].event_type == BLADERF_RF_EVT_RX_EPOCH_INVALID;
        errored |= events[i].event_type == BLADERF_RF_EVT_ERROR;
        valid |= events[i].event_type == BLADERF_RF_EVT_RX_EPOCH_VALID;
        spi_started |= events[i].event_type == BLADERF_RF_EVT_SPI_WRITE_BEGIN;
        spi_done |= events[i].event_type == BLADERF_RF_EVT_SPI_DONE;
        lo_readback |= events[i].event_type == BLADERF_RF_EVT_LO_READBACK_MATCH;
        pll_locked |= events[i].event_type == BLADERF_RF_EVT_RX_PLL_LOCKED;
        ensm_rx |= events[i].event_type == BLADERF_RF_EVT_ENSM_RX;
    }
    if (!invalidated || !errored || valid || spi_started || spi_done ||
        lo_readback || pll_locked || ensm_rx ||
        events[count - 1].event_type != BLADERF_RF_EVT_ERROR ||
        events[count - 1].error_code != BLADERF_ERR_RANGE) {
        fprintf(stderr, "failed transition trace invalid txn=%u "
                "invalidated=%u error=%u epoch_valid=%u spi=%u/%u "
                "readback=%u pll=%u ensm=%u events=%u\n",
                transaction_id, invalidated, errored, valid,
                spi_started, spi_done, lo_readback, pll_locked, ensm_rx, count);
        return BLADERF_ERR_UNEXPECTED;
    }
    return 0;
}

#ifdef BLADERF_ENABLE_TEST_RX_ABORT_FAULT_INJECTION
static int check_abort_failed_trace(struct bladerf *dev,
                                    uint32_t transaction_id)
{
    struct bladerf_rf_event events[BLADERF_RF_EVENT_HISTORY_SIZE];
    uint32_t count = 0;
    bool complete = false;
    bool transition_error = false;
    int status = bladerf_rx_transition_get_events(
        dev, transaction_id, events, BLADERF_RF_EVENT_HISTORY_SIZE,
        &count, &complete);

    if (status != 0 || !complete || count < 2) {
        fprintf(stderr, "ABORT trace query txn=%u status=%d complete=%u count=%u\n",
                transaction_id, status, complete, count);
        return status ? status : BLADERF_ERR_UNEXPECTED;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (events[i].transaction_id != transaction_id) {
            return BLADERF_ERR_UNEXPECTED;
        }
        transition_error |= events[i].event_type == BLADERF_RF_EVT_ERROR;
    }
    if (!transition_error ||
        events[count - 1].event_type != BLADERF_RF_EVT_RX_EPOCH_ABORT_FAILED ||
        events[count - 1].error_code != BLADERF_ERR_UNEXPECTED) {
        fprintf(stderr, "ABORT failure missing from trace txn=%u count=%u "
                "transition_error=%u last=%u error=%d\n", transaction_id,
                count, transition_error, events[count - 1].event_type,
                events[count - 1].error_code);
        for (uint32_t i = 0; i < count; ++i) {
            fprintf(stderr, "  event[%u]=%u error=%d flags=0x%x txn=%u\n",
                    i, events[i].event_type, events[i].error_code,
                    events[i].flags, events[i].transaction_id);
        }
        return BLADERF_ERR_UNEXPECTED;
    }
    return 0;
}
#endif

#ifdef BLADERF_ENABLE_TEST_SPI_FAULT_INJECTION
static int check_injected_spi_failure_trace(struct bladerf *dev,
                                            uint32_t transaction_id)
{
    struct bladerf_rf_event events[BLADERF_RF_EVENT_HISTORY_SIZE];
    uint32_t count = 0;
    bool complete = false;
    bool invalidated = false;
    bool spi_begin = false;
    bool spi_failed = false;
    bool errored = false;
    bool forbidden_success_event = false;
    uint32_t spi_begin_count = 0;
    uint32_t spi_done_count = 0;
    uint64_t previous_timestamp = 0;
    int status = bladerf_rx_transition_get_events(
        dev, transaction_id, events, BLADERF_RF_EVENT_HISTORY_SIZE,
        &count, &complete);

    if (status != 0 || !complete || count == 0) {
        return status ? status : BLADERF_ERR_UNEXPECTED;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const struct bladerf_rf_event *event = &events[i];
        if (event->transaction_id != transaction_id ||
            event->host_monotonic_ns < previous_timestamp) {
            return BLADERF_ERR_UNEXPECTED;
        }
        previous_timestamp = event->host_monotonic_ns;
        invalidated |= event->event_type == BLADERF_RF_EVT_RX_EPOCH_INVALID;
        if (event->event_type == BLADERF_RF_EVT_SPI_WRITE_BEGIN) {
            spi_begin = true;
            spi_begin_count = event->flags;
        }
        if (event->event_type == BLADERF_RF_EVT_SPI_DONE) {
            spi_failed |= event->error_code != 0 &&
                          (int32_t)event->rfic_status < 0;
            spi_done_count = event->flags;
        }
        errored |= event->event_type == BLADERF_RF_EVT_ERROR;
        forbidden_success_event |=
            event->event_type == BLADERF_RF_EVT_LO_SET_RETURNED ||
            event->event_type == BLADERF_RF_EVT_LO_READBACK_MATCH ||
            event->event_type == BLADERF_RF_EVT_RX_PLL_LOCKED ||
            event->event_type == BLADERF_RF_EVT_ENSM_RX ||
            event->event_type == BLADERF_RF_EVT_RX_EPOCH_VALID;
    }

    if (!invalidated || !spi_begin || !spi_failed || !errored ||
        spi_begin_count == 0 || spi_done_count == 0 ||
        forbidden_success_event ||
        events[count - 1].event_type != BLADERF_RF_EVT_ERROR) {
        fprintf(stderr, "SPI fault trace invalid txn=%u events=%u "
                "invalidated=%u spi_begin=%u spi_failed=%u error=%u "
                "success_event=%u spi_flags=%u/%u\n",
                transaction_id, count, invalidated,
                spi_begin, spi_failed, errored, forbidden_success_event,
                spi_begin_count, spi_done_count);
        return BLADERF_ERR_UNEXPECTED;
    }
    return 0;
}
#endif

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

#ifdef BLADERF_ENABLE_TEST_SPI_FAULT_INJECTION
    /* The oversample register script is part of the RX configuration
     * transaction. An injected write failure must reach the caller and keep
     * META RX invalid until the configuration and a new transition succeed. */
    CHECK(bladerf_enable_feature(dev, BLADERF_FEATURE_OVERSAMPLE, true));
    if (setenv("BLADERF_TEST_SPI_FAIL_RFIC_REGISTER", "0x1e2", 1) != 0) {
        status = BLADERF_ERR_UNEXPECTED;
        goto out;
    }
    status = bladerf_set_sample_rate(dev, BLADERF_CHANNEL_RX(0), 6250000,
                                     NULL);
    unsetenv("BLADERF_TEST_SPI_FAIL_RFIC_REGISTER");
    if (status == 0) {
        fprintf(stderr, "oversample RFIC write failure was hidden\n");
        status = BLADERF_ERR_UNEXPECTED;
        goto out;
    }
    struct bladerf_metadata config_failure_meta = {0};
    config_failure_meta.flags = BLADERF_META_FLAG_RX_NOW;
    status = bladerf_sync_rx(dev, samples, 4096, &config_failure_meta, 300);
    if (status != BLADERF_ERR_WOULD_BLOCK ||
        config_failure_meta.actual_count != 0) {
        fprintf(stderr, "partial oversample configuration admitted IQ: "
                "status=%s count=%u\n", bladerf_strerror(status),
                config_failure_meta.actual_count);
        status = BLADERF_ERR_UNEXPECTED;
        goto out;
    }
    status = bladerf_set_sample_rate(dev, BLADERF_CHANNEL_RX(0), 4000000,
                                     NULL);
    if (status == 0) {
        fprintf(stderr, "oversample sample-rate range failure was hidden\n");
        status = BLADERF_ERR_UNEXPECTED;
        goto out;
    }
    memset(&config_failure_meta, 0, sizeof(config_failure_meta));
    config_failure_meta.flags = BLADERF_META_FLAG_RX_NOW;
    status = bladerf_sync_rx(dev, samples, 4096, &config_failure_meta, 300);
    if (status != BLADERF_ERR_WOULD_BLOCK ||
        config_failure_meta.actual_count != 0) {
        fprintf(stderr, "failed sample-rate change admitted IQ: "
                "status=%s count=%u\n", bladerf_strerror(status),
                config_failure_meta.actual_count);
        status = BLADERF_ERR_UNEXPECTED;
        goto out;
    }
    CHECK(bladerf_set_sample_rate(dev, BLADERF_CHANNEL_RX(0), 6250000, NULL));
    struct bladerf_rf_event config_recovered = {0};
    CHECK(transition(dev, recovery_frequency_hz, &config_recovered));
    CHECK(read_valid_epoch(dev, samples, config_recovered.epoch_id,
                           config_recovered.fpga_timestamp));
    initial = config_recovered;
    printf("RX oversample config fault injection: PASS; failed write stayed "
           "invalid until full register retry and fresh epoch %u\n",
           config_recovered.epoch_id);

    const uint32_t fault_ordinals[] = { 1, 5 };
    for (size_t i = 0; i < sizeof(fault_ordinals) / sizeof(fault_ordinals[0]);
         ++i) {
        struct bladerf_rx_transition_request injected_failure_request = {
            .target_frequency_hz = 1835500000ULL + i * 100000ULL,
            .required_events_mask = BLADERF_RF_REQUIRE_PLL_LOCKED |
                                    BLADERF_RF_REQUIRE_ENSM_RX |
                                    BLADERF_RF_REQUIRE_EPOCH_VALID,
            .timeout_ms = 2000,
            .require_rx_data_valid = true,
            .epoch_settle_samples = 0,
        };
        char ordinal[16];
        uint32_t injected_failure_txn = 0;
        snprintf(ordinal, sizeof(ordinal), "%u", fault_ordinals[i]);
        if (setenv("BLADERF_TEST_SPI_FAIL_RX_TRANSITION_WRITE", ordinal, 1) != 0) {
            status = BLADERF_ERR_UNEXPECTED;
            goto out;
        }
        status = bladerf_rx_transition_begin(
            dev, BLADERF_CHANNEL_RX(0), &injected_failure_request,
            &injected_failure_txn);
        unsetenv("BLADERF_TEST_SPI_FAIL_RX_TRANSITION_WRITE");
        if (status == 0 || injected_failure_txn == 0) {
            fprintf(stderr, "injected SPI write %u failure unexpectedly succeeded\n",
                    fault_ordinals[i]);
            status = BLADERF_ERR_UNEXPECTED;
            goto out;
        }
        CHECK(check_injected_spi_failure_trace(dev, injected_failure_txn));

        struct bladerf_metadata failed_metadata = {0};
        failed_metadata.flags = BLADERF_META_FLAG_RX_NOW;
        status = bladerf_sync_rx(dev, samples, 4096, &failed_metadata, 300);
        if (status != BLADERF_ERR_WOULD_BLOCK ||
            failed_metadata.actual_count != 0) {
            fprintf(stderr, "SPI failure admitted IQ: status=%s count=%u\n",
                    bladerf_strerror(status), failed_metadata.actual_count);
            status = BLADERF_ERR_UNEXPECTED;
            goto out;
        }

        struct bladerf_rf_event spi_recovered = {0};
        CHECK(transition(dev, recovery_frequency_hz, &spi_recovered));
        if (spi_recovered.epoch_id == initial.epoch_id) {
            fprintf(stderr, "SPI failure recovery reused old epoch %u\n",
                    initial.epoch_id);
            status = BLADERF_ERR_UNEXPECTED;
            goto out;
        }
        CHECK(read_valid_epoch(dev, samples, spi_recovered.epoch_id,
                               spi_recovered.fpga_timestamp));
        initial = spi_recovered;
        printf("RX SPI fault injection: PASS ordinal=%u failed_txn=%u "
               "recovered_epoch=%u\n", fault_ordinals[i],
               injected_failure_txn, spi_recovered.epoch_id);
    }
#endif

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

    /* The host parser remains invalidated while the FPGA gate is in ERROR.
     * sync_rx must fail promptly rather than wait on an epoch that cannot
     * arrive until the next explicit transition. */
    struct bladerf_metadata metadata = {0};
    metadata.flags = BLADERF_META_FLAG_RX_NOW;
    status = bladerf_sync_rx(dev, samples, 4096, &metadata, 300);
    if (status != BLADERF_ERR_WOULD_BLOCK) {
        fprintf(stderr, "failed epoch read returned %s (epoch=%u valid=%u "
                "count=%u), expected fail-closed WOULD_BLOCK\n",
                bladerf_strerror(status),
                metadata.rx_epoch_id, metadata.rx_epoch_id_valid,
                metadata.actual_count);
        status = BLADERF_ERR_UNEXPECTED;
        goto out;
    }

    /* Keep the recovery reproduction actionable: expose backend events from
     * the moment the failed epoch is followed by a new transition. */
    bladerf_log_set_verbosity(BLADERF_LOG_LEVEL_DEBUG);

#ifdef BLADERF_ENABLE_TEST_RX_ABORT_FAULT_INJECTION
    /* Fail only the NIOS cleanup command after FPGA ARM. The event trace must
     * distinguish failed cleanup, IQ must remain blocked, and the next
     * explicit transition must recover the still-pending FPGA gate. */
    uint32_t abort_failed_transaction_id = 0;
    if (setenv("BLADERF_TEST_FAIL_RX_EPOCH_ABORT", "1", 1) != 0) {
        status = BLADERF_ERR_UNEXPECTED;
        goto out;
    }
    status = bladerf_rx_transition_begin(
        dev, BLADERF_CHANNEL_RX(0), &invalid_request,
        &abort_failed_transaction_id);
    unsetenv("BLADERF_TEST_FAIL_RX_EPOCH_ABORT");
    if (status != BLADERF_ERR_RANGE || abort_failed_transaction_id == 0) {
        fprintf(stderr, "ABORT-fault transition returned %s txn=%u\n",
                bladerf_strerror(status), abort_failed_transaction_id);
        status = BLADERF_ERR_UNEXPECTED;
        goto out;
    }
    CHECK(check_abort_failed_trace(dev, abort_failed_transaction_id));

    memset(&metadata, 0, sizeof(metadata));
    metadata.flags = BLADERF_META_FLAG_RX_NOW;
    status = bladerf_sync_rx(dev, samples, 4096, &metadata, 300);
    if (status != BLADERF_ERR_WOULD_BLOCK || metadata.actual_count != 0) {
        fprintf(stderr, "failed NIOS ABORT admitted IQ: status=%s count=%u\n",
                bladerf_strerror(status), metadata.actual_count);
        status = BLADERF_ERR_UNEXPECTED;
        goto out;
    }
    printf("RX epoch ABORT fault: PASS txn=%u IQ withheld; recovery follows\n",
           abort_failed_transaction_id);
#endif

    CHECK(transition(dev, recovery_frequency_hz, &recovered));
    if (recovered.epoch_id == initial.epoch_id) {
        fprintf(stderr, "recovery reused epoch ID %u\n", recovered.epoch_id);
        status = BLADERF_ERR_UNEXPECTED;
        goto out;
    }
    CHECK(read_valid_epoch(dev, samples, recovered.epoch_id,
                           recovered.fpga_timestamp));

#ifdef BLADERF_ENABLE_TEST_RX_ABORT_FAULT_INJECTION
    /* Legacy setters must not mutate the RFIC when host revocation succeeds
     * but the FPGA ABORT command fails. Capture the event cursor first so
     * this checks the exact operation, even when prior transitions filled
     * the bounded history ring. */
    int gain_before = 0;
    int gain_after = 0;
    CHECK(bladerf_get_gain(dev, BLADERF_CHANNEL_RX(0), &gain_before));
    struct bladerf_rf_event recent[BLADERF_RF_EVENT_HISTORY_SIZE];
    uint32_t recent_count = 0;
    uint64_t event_cursor = 0;
    bool history_complete = false;
    int cursor_status = bladerf_rf_events_get_since(
        dev, 0, recent, BLADERF_RF_EVENT_HISTORY_SIZE, &recent_count,
        &event_cursor, &history_complete);
    if (cursor_status != 0 && cursor_status != BLADERF_ERR_MEM) {
        status = cursor_status;
        goto out;
    }

    if (setenv("BLADERF_TEST_FAIL_RX_EPOCH_ABORT", "1", 1) != 0) {
        status = BLADERF_ERR_UNEXPECTED;
        goto out;
    }
    status = bladerf_set_gain(dev, BLADERF_CHANNEL_RX(0), gain_before + 1);
    unsetenv("BLADERF_TEST_FAIL_RX_EPOCH_ABORT");
    if (status != BLADERF_ERR_UNEXPECTED) {
        fprintf(stderr, "setter with failed FPGA ABORT returned %s\n",
                bladerf_strerror(status));
        status = BLADERF_ERR_UNEXPECTED;
        goto out;
    }

    recent_count = 0;
    uint64_t next_event_cursor = event_cursor;
    CHECK(bladerf_rf_events_get_since(
        dev, event_cursor, recent, BLADERF_RF_EVENT_HISTORY_SIZE,
        &recent_count, &next_event_cursor, &history_complete));
    bool gain_invalidated = false;
    bool abort_failure_reported = false;
    for (uint32_t i = 0; i < recent_count; ++i) {
        gain_invalidated |=
            recent[i].event_type == BLADERF_RF_EVT_RX_DATA_INVALIDATED &&
            recent[i].flags == BLADERF_RF_INVALIDATE_GAIN;
        abort_failure_reported |=
            recent[i].event_type == BLADERF_RF_EVT_RX_EPOCH_ABORT_FAILED &&
            recent[i].error_code == BLADERF_ERR_UNEXPECTED;
    }
    CHECK(bladerf_get_gain(dev, BLADERF_CHANNEL_RX(0), &gain_after));
    if (!history_complete || !gain_invalidated || !abort_failure_reported ||
        gain_after != gain_before) {
        fprintf(stderr, "failed setter ABORT contract mismatch: complete=%u "
                "invalidated=%u abort_event=%u gain=%d->%d events=%u\n",
                history_complete, gain_invalidated, abort_failure_reported,
                gain_before, gain_after, recent_count);
        status = BLADERF_ERR_UNEXPECTED;
        goto out;
    }

    int16_t sentinel[4096 * 2];
    memset(samples, 0x5a, 4096 * 2 * sizeof(*samples));
    memcpy(sentinel, samples, sizeof(sentinel));
    memset(&metadata, 0, sizeof(metadata));
    metadata.flags = BLADERF_META_FLAG_RX_NOW;
    status = bladerf_sync_rx(dev, samples, 4096, &metadata, 300);
    if (status != BLADERF_ERR_WOULD_BLOCK || metadata.actual_count != 0 ||
        memcmp(samples, sentinel, sizeof(sentinel)) != 0) {
        fprintf(stderr, "failed setter ABORT exposed data: status=%s count=%u\n",
                bladerf_strerror(status), metadata.actual_count);
        status = BLADERF_ERR_UNEXPECTED;
        goto out;
    }

    struct bladerf_rf_event setter_recovered = {0};
    CHECK(transition(dev, recovery_frequency_hz, &setter_recovered));
    CHECK(read_valid_epoch(dev, samples, setter_recovered.epoch_id,
                           setter_recovered.fpga_timestamp));
    recovered = setter_recovered;
    printf("RX legacy setter ABORT fault: PASS gain unchanged, IQ withheld, "
           "explicit epoch recovery=%u\n", recovered.epoch_id);
#endif

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
