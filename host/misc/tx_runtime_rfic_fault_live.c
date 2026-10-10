#define _DEFAULT_SOURCE

/* Test-build xA4 qualification for TX RFIC monitor invalidation. Build with
 * ENABLE_TEST_RX_TRANSITION_STALL_INJECTION=ON and set
 * BLADERF_TEST_RX_TRANSITION_STALL to one of:
 *   RUNTIME_TX_RFIC_STATUS_READ_FAILURE
 *   RUNTIME_TX_RFPLL_UNLOCKED
 *   RUNTIME_TX_ENSM_NOT_TX
 *   RUNTIME_TX_BBPLL_UNLOCKED
 *   RUNTIME_TX_LINK_FAULT_NO_RFIC_READ
 *   RUNTIME_TX_RETUNE_FAILURE
 *   RUNTIME_TX_RFIC_REGISTER_WRITE
 *   RUNTIME_TX_QUEUE_CLEAR_FAILURE
 *   RUNTIME_TX_GATE_STATUS_READ_FAILURE
 *   RUNTIME_TX_GATE_RELEASE_FAILURE
 *   RUNTIME_TX_GATE_ABORT_FAILURE
 *
 * The test verifies durable event classification and that sync TX refuses
 * samples after the monitor or a legacy RF configuration setter revokes
 * validity. */
#include <libbladeRF.h>

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define TX_SAMPLES 4096u
#define EVENT_WAIT_MS 4000u
#define TX_FEEDER_TIMEOUT_MS 50u

struct tx_feeder {
    struct bladerf *dev;
    int16_t samples[TX_SAMPLES * 2];
    atomic_bool stop;
    atomic_int terminal_status;
};

static void *feed_tx_until_fault(void *arg)
{
    struct tx_feeder *feeder = arg;
    while (!atomic_load(&feeder->stop)) {
        const int status = bladerf_sync_tx(
            feeder->dev, feeder->samples, TX_SAMPLES, NULL,
            TX_FEEDER_TIMEOUT_MS);
        if (status == 0 || status == BLADERF_ERR_TIMEOUT ||
            status == BLADERF_ERR_WOULD_BLOCK) {
            continue;
        }
        atomic_store(&feeder->terminal_status, status);
        break;
    }
    return NULL;
}

static uint64_t monotonic_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000u +
           (uint64_t)now.tv_nsec / 1000000u;
}

static int wait_for_event(struct bladerf *dev,
                          struct bladerf_rf_event *events,
                          uint64_t *cursor,
                          bladerf_rf_event_type expected,
                          uint32_t timeout_ms)
{
    const uint64_t deadline = monotonic_ms() + timeout_ms;

    while (monotonic_ms() < deadline) {
        uint64_t next = *cursor;
        uint32_t count = 0;
        bool history_complete = false;
        int status = bladerf_rf_events_get_since(
            dev, *cursor, events, BLADERF_RF_EVENT_HISTORY_SIZE,
            &count, &next, &history_complete);
        if (status != 0 || !history_complete) {
            return status != 0 ? status : BLADERF_ERR_UNEXPECTED;
        }
        *cursor = next;
        for (uint32_t i = 0; i < count; ++i) {
            if (events[i].event_type == expected) {
                return 0;
            }
        }
        const struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000};
        nanosleep(&pause, NULL);
    }
    return BLADERF_ERR_TIMEOUT;
}

int main(void)
{
    struct bladerf *dev = NULL;
    struct tx_feeder feeder = {0};
    pthread_t feeder_thread;
    struct bladerf_rf_event events[BLADERF_RF_EVENT_HISTORY_SIZE];
    const char *mode = getenv("BLADERF_TEST_RX_TRANSITION_STALL");
    uint64_t cursor = 0;
    uint64_t deadline;
    uint32_t count = 0;
    bool history_complete = false;
    bool expected_event_seen = false;
    bool link_fault_seen = false;
    bool rfics_invalid_seen = false;
    bool feeder_started = false;
    int status;

    if (mode == NULL) {
        fprintf(stderr, "set BLADERF_TEST_RX_TRANSITION_STALL to a TX mode\n");
        return 2;
    }
    if (strcmp(mode, "RUNTIME_TX_RFIC_STATUS_READ_FAILURE") != 0 &&
        strcmp(mode, "RUNTIME_TX_RFPLL_UNLOCKED") != 0 &&
        strcmp(mode, "RUNTIME_TX_ENSM_NOT_TX") != 0 &&
        strcmp(mode, "RUNTIME_TX_BBPLL_UNLOCKED") != 0 &&
        strcmp(mode, "RUNTIME_TX_LINK_FAULT_NO_RFIC_READ") != 0 &&
        strcmp(mode, "RUNTIME_TX_RETUNE_FAILURE") != 0 &&
        strcmp(mode, "RUNTIME_TX_RFIC_REGISTER_WRITE") != 0 &&
        strcmp(mode, "RUNTIME_TX_QUEUE_CLEAR_FAILURE") != 0 &&
        strcmp(mode, "RUNTIME_TX_GATE_STATUS_READ_FAILURE") != 0 &&
        strcmp(mode, "RUNTIME_TX_GATE_RELEASE_FAILURE") != 0 &&
        strcmp(mode, "RUNTIME_TX_GATE_ABORT_FAILURE") != 0) {
        fprintf(stderr, "unsupported TX runtime injection mode: %s\n", mode);
        return 2;
    }

    status = bladerf_open(&dev, NULL);
    if (status != 0) goto cleanup;
    bladerf_log_set_verbosity(BLADERF_LOG_LEVEL_WARNING);
    status = bladerf_set_sample_rate(dev, BLADERF_CHANNEL_TX(0),
                                     1000000, NULL);
    if (status != 0) goto cleanup;
    status = bladerf_set_bandwidth(dev, BLADERF_CHANNEL_TX(0),
                                   1500000, NULL);
    if (status != 0) goto cleanup;
    status = bladerf_sync_config(dev, BLADERF_TX_X1,
                                 BLADERF_FORMAT_SC16_Q11,
                                 16, 8192, 8, 1000);
    if (status != 0) goto cleanup;

    status = bladerf_enable_module(dev, BLADERF_CHANNEL_TX(0), true);
    if (status != 0) goto cleanup;
    if (strcmp(mode, "RUNTIME_TX_QUEUE_CLEAR_FAILURE") == 0 ||
        strcmp(mode, "RUNTIME_TX_GATE_STATUS_READ_FAILURE") == 0 ||
        strcmp(mode, "RUNTIME_TX_GATE_RELEASE_FAILURE") == 0 ||
        strcmp(mode, "RUNTIME_TX_GATE_ABORT_FAILURE") == 0) {
        status = bladerf_set_frequency(dev, BLADERF_CHANNEL_TX(0),
                                       915000000);
        if (status == 0) {
            status = wait_for_event(dev, events, &cursor,
                                    BLADERF_RF_EVT_TX_RETUNE_VALID,
                                    EVENT_WAIT_MS);
        }
        if (status != 0) {
            fprintf(stderr, "could not establish TX frequency baseline: %s\n",
                    bladerf_strerror(status));
            goto cleanup;
        }
    }
    feeder.dev = dev;
    atomic_init(&feeder.stop, false);
    atomic_init(&feeder.terminal_status, 0);
    status = pthread_create(&feeder_thread, NULL, feed_tx_until_fault,
                            &feeder);
    if (status != 0) {
        status = BLADERF_ERR_UNEXPECTED;
        goto cleanup;
    }
    feeder_started = true;

    if (strcmp(mode, "RUNTIME_TX_QUEUE_CLEAR_FAILURE") == 0) {
        bladerf_frequency frequency = 0;
        bladerf_timestamp timestamp = 0;
        struct bladerf_quick_tune quick_tune;

        status = bladerf_get_frequency(dev, BLADERF_CHANNEL_TX(0),
                                       &frequency);
        if (status == 0) {
            status = bladerf_get_quick_tune(dev, BLADERF_CHANNEL_TX(0),
                                            &quick_tune);
        }
        if (status == 0) {
            status = bladerf_get_timestamp(dev, BLADERF_TX, &timestamp);
        }
        if (status == 0) {
            status = bladerf_schedule_retune(
                dev, BLADERF_CHANNEL_TX(0), timestamp + 100000,
                frequency, &quick_tune);
        }
        if (status == 0) {
            status = wait_for_event(dev, events, &cursor,
                                    BLADERF_RF_EVT_TX_RETUNE_VALID,
                                    EVENT_WAIT_MS);
        }
        if (status != 0) {
            fprintf(stderr, "could not establish TX retune baseline: %s\n",
                    bladerf_strerror(status));
            goto cleanup;
        }

        /* The test-build hook fails queue cancellation after a known-good
         * scheduled tune. Direct tuning must fail, report lost completion,
         * and reject further samples until a later acknowledged retune. */
        status = bladerf_set_frequency(dev, BLADERF_CHANNEL_TX(0), frequency);
        if (status != BLADERF_ERR_IO) {
            fprintf(stderr, "queue-clear fault returned %s (%d), expected IO\n",
                    bladerf_strerror(status), status);
            status = BLADERF_ERR_UNEXPECTED;
            goto cleanup;
        }
        status = wait_for_event(dev, events, &cursor,
                                BLADERF_RF_EVT_TX_RETUNE_COMPLETION_LOST,
                                EVENT_WAIT_MS);
        if (status != 0) {
            fprintf(stderr, "queue-clear failure event missing: %s\n",
                    bladerf_strerror(status));
            goto cleanup;
        }
        status = bladerf_sync_tx(dev, feeder.samples, TX_SAMPLES, NULL,
                                 TX_FEEDER_TIMEOUT_MS);
        if (status != BLADERF_ERR_FPGA_OP) {
            fprintf(stderr, "TX samples were not rejected after queue-clear fault: %s\n",
                    bladerf_strerror(status));
            status = BLADERF_ERR_UNEXPECTED;
            goto cleanup;
        }

        if (setenv("BLADERF_TEST_RX_TRANSITION_STALL", "", 1) != 0) {
            status = BLADERF_ERR_UNEXPECTED;
            goto cleanup;
        }
        status = bladerf_set_frequency(dev, BLADERF_CHANNEL_TX(0), frequency);
        if (status == 0) {
            status = wait_for_event(dev, events, &cursor,
                                    BLADERF_RF_EVT_TX_RETUNE_VALID,
                                    EVENT_WAIT_MS);
        }
        if (status == 0) {
            status = bladerf_sync_tx(dev, feeder.samples, TX_SAMPLES, NULL,
                                     TX_FEEDER_TIMEOUT_MS);
        }
        if (status != 0) {
            fprintf(stderr, "TX did not recover after queue-clear fault: %s\n",
                    bladerf_strerror(status));
            goto cleanup;
        }

        printf("TX queue-clear containment: PASS (fault rejected, retuned, resumed)\n");
        status = 0;
        goto cleanup;
    }

    if (strcmp(mode, "RUNTIME_TX_GATE_STATUS_READ_FAILURE") == 0 ||
        strcmp(mode, "RUNTIME_TX_GATE_RELEASE_FAILURE") == 0 ||
        strcmp(mode, "RUNTIME_TX_GATE_ABORT_FAILURE") == 0) {
        bladerf_frequency frequency = 0;
        bladerf_timestamp timestamp = 0;
        struct bladerf_quick_tune quick_tune;

        status = bladerf_get_frequency(dev, BLADERF_CHANNEL_TX(0),
                                       &frequency);
        if (status == 0) {
            status = bladerf_get_quick_tune(dev, BLADERF_CHANNEL_TX(0),
                                            &quick_tune);
        }
        if (status == 0) {
            status = bladerf_get_timestamp(dev, BLADERF_TX, &timestamp);
        }
        if (status == 0 && setenv("BLADERF_TEST_TX_GATE_FAULT", mode, 1) != 0) {
            status = BLADERF_ERR_UNEXPECTED;
        }
        if (status == 0) {
            status = bladerf_schedule_retune(
                dev, BLADERF_CHANNEL_TX(0), timestamp + 100000,
                frequency, &quick_tune);
        }
        if (status == 0) {
            status = wait_for_event(dev, events, &cursor,
                                    BLADERF_RF_EVT_TX_RETUNE_COMPLETION_LOST,
                                    EVENT_WAIT_MS);
        }
        if (status != 0) {
            fprintf(stderr, "TX gate fault was not contained: %s\n",
                    bladerf_strerror(status));
            goto cleanup;
        }
        if (strcmp(mode, "RUNTIME_TX_GATE_ABORT_FAILURE") == 0) {
            status = wait_for_event(dev, events, &cursor,
                                    BLADERF_RF_EVT_TX_STATUS_UNAVAILABLE,
                                    EVENT_WAIT_MS);
            if (status != 0) {
                fprintf(stderr, "failed mute acknowledgement was not reported\n");
                goto cleanup;
            }
            status = bladerf_set_frequency(dev, BLADERF_CHANNEL_TX(0),
                                           frequency);
            if (status != BLADERF_ERR_WOULD_BLOCK) {
                fprintf(stderr, "TX retune was not blocked without mute ACK: %s\n",
                        bladerf_strerror(status));
                status = BLADERF_ERR_UNEXPECTED;
                goto cleanup;
            }
            status = bladerf_set_bandwidth(dev, BLADERF_CHANNEL_TX(0),
                                           1500000, NULL);
            if (status != BLADERF_ERR_WOULD_BLOCK) {
                fprintf(stderr, "TX RF configuration bypassed missing mute ACK: %s\n",
                        bladerf_strerror(status));
                status = BLADERF_ERR_UNEXPECTED;
                goto cleanup;
            }
            status = bladerf_sync_tx(dev, feeder.samples, TX_SAMPLES, NULL,
                                     TX_FEEDER_TIMEOUT_MS);
            if (status != BLADERF_ERR_FPGA_OP) {
                fprintf(stderr, "TX samples were not rejected without mute ACK\n");
                status = BLADERF_ERR_UNEXPECTED;
                goto cleanup;
            }
            printf("TX gate abort failure: PASS (status unavailable, retune blocked)\n");
            status = 0;
            goto cleanup;
        }
        status = bladerf_sync_tx(dev, feeder.samples, TX_SAMPLES, NULL,
                                 TX_FEEDER_TIMEOUT_MS);
        if (status != BLADERF_ERR_FPGA_OP) {
            fprintf(stderr, "TX samples were not rejected after gate fault: %s\n",
                    bladerf_strerror(status));
            status = BLADERF_ERR_UNEXPECTED;
            goto cleanup;
        }
        if (setenv("BLADERF_TEST_TX_GATE_FAULT", "", 1) != 0) {
            status = BLADERF_ERR_UNEXPECTED;
            goto cleanup;
        }
        status = bladerf_set_frequency(dev, BLADERF_CHANNEL_TX(0), frequency);
        if (status == 0) {
            status = wait_for_event(dev, events, &cursor,
                                    BLADERF_RF_EVT_TX_RETUNE_VALID,
                                    EVENT_WAIT_MS);
        }
        if (status == 0) {
            status = bladerf_sync_tx(dev, feeder.samples, TX_SAMPLES, NULL,
                                     TX_FEEDER_TIMEOUT_MS);
        }
        if (status != 0) {
            fprintf(stderr, "TX did not recover after gate fault: %s\n",
                    bladerf_strerror(status));
            goto cleanup;
        }
        printf("TX gate containment: PASS mode=%s (abort, retune, resume)\n",
               mode);
        status = 0;
        goto cleanup;
    }

    if (strcmp(mode, "RUNTIME_TX_RFIC_REGISTER_WRITE") == 0) {
        uint8_t tx_clock_data_delay;
        /* Rewrite the existing value while TX is live. The RFIC value stays
         * unchanged; the public raw-register path must still revoke TX
         * admission and acknowledge the FPGA mute before the SPI write. */
        status = bladerf_get_rfic_register(dev, 0x007,
                                           &tx_clock_data_delay);
        if (status == 0) {
            status = bladerf_set_rfic_register(dev, 0x007,
                                               tx_clock_data_delay);
        }
        if (status != 0) goto cleanup;
    }

    deadline = monotonic_ms() + EVENT_WAIT_MS;
    while (monotonic_ms() < deadline && !expected_event_seen) {
        uint64_t next = cursor;
        status = bladerf_rf_events_get_since(
            dev, cursor, events, BLADERF_RF_EVENT_HISTORY_SIZE,
            &count, &next, &history_complete);
        if (status != 0 || !history_complete) {
            fprintf(stderr, "RF event history incomplete: %s\n",
                    bladerf_strerror(status));
            status = status != 0 ? status : BLADERF_ERR_UNEXPECTED;
            goto cleanup;
        }
        cursor = next;
        for (uint32_t i = 0; i < count; ++i) {
            const struct bladerf_rf_event *event = &events[i];
            if (event->event_type == BLADERF_RF_EVT_TX_LINK_FAULT) {
                link_fault_seen = true;
            } else if (event->event_type ==
                       BLADERF_RF_EVT_TX_RFIC_STATE_INVALID) {
                rfics_invalid_seen = true;
            }

            if ((strcmp(mode, "RUNTIME_TX_RFIC_STATUS_READ_FAILURE") == 0 ||
                 strcmp(mode, "RUNTIME_TX_LINK_FAULT_NO_RFIC_READ") == 0) &&
                event->event_type == BLADERF_RF_EVT_TX_STATUS_UNAVAILABLE) {
                expected_event_seen = true;
            } else if ((strcmp(mode, "RUNTIME_TX_RFPLL_UNLOCKED") == 0 ||
                        strcmp(mode, "RUNTIME_TX_ENSM_NOT_TX") == 0 ||
                        strcmp(mode, "RUNTIME_TX_BBPLL_UNLOCKED") == 0) &&
                       event->event_type ==
                           BLADERF_RF_EVT_TX_RFIC_STATE_INVALID) {
                expected_event_seen = true;
            } else if (strcmp(mode, "RUNTIME_TX_RETUNE_FAILURE") == 0 &&
                       event->event_type ==
                           BLADERF_RF_EVT_TX_RETUNE_FAILED) {
                expected_event_seen = true;
            } else if (strcmp(mode, "RUNTIME_TX_RFIC_REGISTER_WRITE") == 0 &&
                       event->event_type ==
                           BLADERF_RF_EVT_TX_CONFIGURATION_INVALID) {
                expected_event_seen = true;
            }
        }
        if (!expected_event_seen) {
            const struct timespec pause = {.tv_sec = 0,
                                           .tv_nsec = 1000000};
            nanosleep(&pause, NULL);
        }
    }

    if (!expected_event_seen) {
        fprintf(stderr, "expected TX fault event not observed for %s\n", mode);
        status = BLADERF_ERR_UNEXPECTED;
        goto cleanup;
    }
    if (strcmp(mode, "RUNTIME_TX_LINK_FAULT_NO_RFIC_READ") == 0 &&
        (!link_fault_seen || rfics_invalid_seen)) {
        fprintf(stderr, "skipped RFIC reads were misclassified: link_fault=%u "
                "rfic_invalid=%u\n", link_fault_seen, rfics_invalid_seen);
        status = BLADERF_ERR_UNEXPECTED;
        goto cleanup;
    }

    deadline = monotonic_ms() + 1000u;
    while (monotonic_ms() < deadline &&
           atomic_load(&feeder.terminal_status) == 0) {
        const struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000};
        nanosleep(&pause, NULL);
    }
    status = atomic_load(&feeder.terminal_status);
    if (status != BLADERF_ERR_FPGA_OP) {
        fprintf(stderr, "TX feeder was not rejected after fault: %s (%d)\n",
                status != 0 ? bladerf_strerror(status) : "still running",
                status);
        status = BLADERF_ERR_UNEXPECTED;
        goto cleanup;
    }

    if (strcmp(mode, "RUNTIME_TX_RFIC_REGISTER_WRITE") == 0) {
        bladerf_frequency current_frequency = 0;
        bool retune_valid_seen = false;

        /* The configuration fence must be recoverable through a normal TX
         * retune after the FPGA acknowledged mute; it must not require a
         * module disable/enable cycle. */
        status = bladerf_get_frequency(dev, BLADERF_CHANNEL_TX(0),
                                       &current_frequency);
        if (status == 0) {
            status = bladerf_set_frequency(dev, BLADERF_CHANNEL_TX(0),
                                           current_frequency);
        }
        if (status != 0) goto cleanup;

        deadline = monotonic_ms() + EVENT_WAIT_MS;
        while (monotonic_ms() < deadline && !retune_valid_seen) {
            uint64_t next = cursor;
            status = bladerf_rf_events_get_since(
                dev, cursor, events, BLADERF_RF_EVENT_HISTORY_SIZE,
                &count, &next, &history_complete);
            if (status != 0 || !history_complete) {
                status = status != 0 ? status : BLADERF_ERR_UNEXPECTED;
                goto cleanup;
            }
            cursor = next;
            for (uint32_t i = 0; i < count; ++i) {
                retune_valid_seen |= events[i].event_type ==
                                     BLADERF_RF_EVT_TX_RETUNE_VALID;
            }
            if (!retune_valid_seen) {
                const struct timespec pause = {.tv_sec = 0,
                                               .tv_nsec = 1000000};
                nanosleep(&pause, NULL);
            }
        }
        if (!retune_valid_seen) {
            fprintf(stderr, "TX did not recover through a new retune\n");
            status = BLADERF_ERR_UNEXPECTED;
            goto cleanup;
        }

        status = bladerf_sync_tx(dev, feeder.samples, TX_SAMPLES, NULL,
                                 TX_FEEDER_TIMEOUT_MS);
        if (status != 0) {
            fprintf(stderr, "TX remained blocked after valid retune: %s\n",
                    bladerf_strerror(status));
            goto cleanup;
        }
    }

    printf("TX RFIC fault path: PASS mode=%s link_fault=%u "
           "rfic_invalid=%u sync_tx=REJECTED\n",
           mode, link_fault_seen, rfics_invalid_seen);
    status = 0;

cleanup:
    unsetenv("BLADERF_TEST_RX_TRANSITION_STALL");
    unsetenv("BLADERF_TEST_TX_GATE_FAULT");
    if (feeder_started) {
        atomic_store(&feeder.stop, true);
        (void)pthread_join(feeder_thread, NULL);
    }
    if (dev != NULL) {
        (void)bladerf_enable_module(dev, BLADERF_CHANNEL_TX(0), false);
        bladerf_close(dev);
    }
    if (status != 0) {
        fprintf(stderr, "TX RFIC fault test failed: %s (%d)\n",
                bladerf_strerror(status), status);
    }
    return status == 0 ? 0 : 1;
}
