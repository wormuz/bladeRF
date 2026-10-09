/*
 * This file is part of the bladeRF project:
 *   http://www.github.com/nuand/bladeRF
 *
 * Copyright (C) 2024 Nuand LLC
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * This program is intended to verify that C programs build against
 * libbladeRF without any unintended dependencies.
 */
#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <getopt.h>
#include <libbladeRF.h>

#define CHECK_STATUS(fn) \
    do { \
        status = (fn); \
        if (status != 0) { \
            fprintf(stderr, "Error at %s:%d (%s): Status %d\n", __FILE__, __LINE__, __func__, status); \
            goto error; \
        } \
    } while (0)

static int verify_recovered_rx_event_chain(struct bladerf *dev)
{
    int status;
    int disable_status;
    bool module_enabled = false;
    int16_t samples[8192] = {0};
    struct bladerf_metadata metadata = {0};
    struct bladerf_rx_transition_request request = {
        .target_frequency_hz = 1835000000ULL,
        .required_events_mask = BLADERF_RF_REQUIRE_PLL_LOCKED |
            BLADERF_RF_REQUIRE_ENSM_RX |
            BLADERF_RF_REQUIRE_EPOCH_VALID |
            BLADERF_RF_REQUIRE_FIRST_HOST_DATA,
        .timeout_ms = 2000,
        .require_rx_data_valid = true,
        .epoch_settle_samples = 0,
    };
    struct bladerf_rf_event event = {0};
    uint32_t transaction_id = 0;

    status = bladerf_set_sample_rate(
        dev, BLADERF_CHANNEL_RX(0), 4000000, NULL);
    if (status != 0) goto done;
    status = bladerf_set_bandwidth(
        dev, BLADERF_CHANNEL_RX(0), 5000000, NULL);
    if (status != 0) goto done;
    status = bladerf_sync_config(dev, BLADERF_RX_X1,
                                 BLADERF_FORMAT_SC16_Q11_META,
                                 16, 4096, 8, 3000);
    if (status != 0) goto done;
    status = bladerf_enable_module(dev, BLADERF_CHANNEL_RX(0), true);
    if (status != 0) goto done;
    module_enabled = true;
    status = bladerf_rx_transition_begin(
        dev, BLADERF_CHANNEL_RX(0), &request, &transaction_id);
    if (status != 0) goto done;
    status = bladerf_rx_transition_wait(
        dev, transaction_id, &event, request.timeout_ms);
    if (status != 0) goto done;
    if (event.event_type != BLADERF_RF_EVT_RX_FIRST_VALID_HOST_DATA) {
        fprintf(stderr, "Expected first-host-data event, got type %u\n",
                (unsigned)event.event_type);
        status = BLADERF_ERR_UNEXPECTED;
        goto done;
    }

    metadata.flags = BLADERF_META_FLAG_RX_NOW;
    status = bladerf_sync_rx(dev, samples, 4096, &metadata, 5000);
    if (status != 0) goto done;
    if (!metadata.rx_epoch_id_valid ||
            metadata.rx_epoch_id != event.epoch_id ||
            metadata.actual_count != 4096 ||
            (metadata.status & BLADERF_META_STATUS_OVERRUN) ||
            metadata.timestamp < event.fpga_timestamp) {
        fprintf(stderr, "Post-reload RX META failed epoch admission: "
                "epoch=%u event_epoch=%u valid=%u count=%u status=0x%x "
                "timestamp=%llu boundary=%llu\n",
                metadata.rx_epoch_id, event.epoch_id,
                metadata.rx_epoch_id_valid, metadata.actual_count,
                metadata.status, (unsigned long long)metadata.timestamp,
                (unsigned long long)event.fpga_timestamp);
        status = BLADERF_ERR_UNEXPECTED;
        goto done;
    }

    printf("POST_RECOVERY_RX_VALID txn=%u epoch=%u timestamp=%llu count=%u\n",
           transaction_id, event.epoch_id,
           (unsigned long long)metadata.timestamp, metadata.actual_count);
    status = bladerf_rx_capture_close(dev, BLADERF_CHANNEL_RX(0));

done:
    if (module_enabled) {
        disable_status = bladerf_enable_module(
            dev, BLADERF_CHANNEL_RX(0), false);
        if (status == 0) status = disable_status;
    }
    return status;
}

#ifdef BLADERF_ENABLE_TEST_FPGA_LOAD_FAILURE_INJECTION
static int set_fpga_load_failure_injection(int enabled,
                                          int report_unconfigured)
{
#ifdef _WIN32
    int status = _putenv_s("BLADERF_TEST_FAIL_FPGA_RELOAD_ONCE",
                           enabled ? "1" : "");
    if (status == 0) {
        status = _putenv_s("BLADERF_TEST_FPGA_RELOAD_REPORT_UNCONFIGURED",
                           report_unconfigured ? "1" : "");
    }
    return status;
#else
    int status = enabled
        ? setenv("BLADERF_TEST_FAIL_FPGA_RELOAD_ONCE", "1", 1)
        : unsetenv("BLADERF_TEST_FAIL_FPGA_RELOAD_ONCE");
    if (status == 0) {
        status = report_unconfigured
            ? setenv("BLADERF_TEST_FPGA_RELOAD_REPORT_UNCONFIGURED", "1", 1)
            : unsetenv("BLADERF_TEST_FPGA_RELOAD_REPORT_UNCONFIGURED");
    }
    return status;
#endif
}

static int set_fpga_backend_failure_injection(int enabled)
{
#ifdef _WIN32
    return _putenv_s("BLADERF_TEST_FAIL_FPGA_BULK_ONCE",
                     enabled ? "1" : "");
#else
    return enabled
        ? setenv("BLADERF_TEST_FAIL_FPGA_BULK_ONCE", "1", 1)
        : unsetenv("BLADERF_TEST_FAIL_FPGA_BULK_ONCE");
#endif
}
#endif

int main(int argc, char *argv[])
{
    int status;
    struct bladerf *dev = NULL;
    const char *fpga_file = "latest.rbf";
    const char *device_string = NULL;
    int reload_only = 0;
    int failed_reload_only = 0;
    int failed_backend_reload_only = 0;
    int report_unconfigured = 0;
    int c;

    bladerf_log_set_verbosity(BLADERF_LOG_LEVEL_ERROR);

    while (1) {
        int option_index = 0;
        static struct option long_options[] = {
            {"device",      required_argument   , 0, 'd'},
            {"fpga",        required_argument   , 0, 'f'},
            {"verbosity",   no_argument         , 0, 'v'},
            {"reload-only", no_argument         , 0, 'r'},
            {"failed-reload-only", no_argument  , 0, 'F'},
            {"failed-reload-unconfigured-only", no_argument, 0, 'U'},
            {"failed-backend-reload-only", no_argument, 0, 'B'},
            {0, 0, 0, 0 }
        };

        c = getopt_long(argc, argv, "d:f:vrFUB", long_options, &option_index);
        if (c == -1)
            break;

        switch (c) {
            case 'd':
                device_string = optarg;
                printf("Using device: %s\n", optarg);
                break;

            case 'f':
                fpga_file = optarg;
                printf("Using FPGA file: %s\n", fpga_file);
                break;

            case 'v':
                printf("Setting verbosity to verbose\n");
                bladerf_log_set_verbosity(BLADERF_LOG_LEVEL_VERBOSE);
                break;

            case 'r':
                reload_only = 1;
                printf("Testing same-handle FPGA reload lifecycle only\n");
                break;

            case 'F':
                reload_only = 1;
                failed_reload_only = 1;
                printf("Testing failed same-handle FPGA reload recovery\n");
                break;

            case 'U':
                reload_only = 1;
                failed_reload_only = 1;
                report_unconfigured = 1;
                printf("Testing fail-closed state after unconfigured FPGA report\n");
                break;

            case 'B':
                reload_only = 1;
                failed_backend_reload_only = 1;
                report_unconfigured = 1;
                printf("Testing recovery after partial FPGA bitstream transfer\n");
                break;

            case '?':
                // getopt_long already printed an error message.
                exit(EXIT_FAILURE);

            default:
                printf("?? getopt returned character code 0%o ??\n", c);
        }
    }

    if (device_string == NULL) {
        fprintf(stderr, "No device specified.\n");
        exit(EXIT_FAILURE);
    }

    printf("Opening device...\n");
    CHECK_STATUS(bladerf_open(&dev, device_string));

    printf("Loading FPGA image...\n");
    CHECK_STATUS(bladerf_load_fpga(dev, fpga_file));

    printf("Setting sample rate to 10e6...\n");
    CHECK_STATUS(bladerf_set_sample_rate(dev, BLADERF_MODULE_RX, 10e6, NULL));

    if (failed_reload_only || failed_backend_reload_only) {
        printf("Injecting one FPGA reload failure, then checking recovery...\n");
#ifdef BLADERF_ENABLE_TEST_FPGA_LOAD_FAILURE_INJECTION
        int arm_status = failed_backend_reload_only
            ? set_fpga_backend_failure_injection(1)
            : set_fpga_load_failure_injection(1, report_unconfigured);
        if (arm_status != 0) {
            fprintf(stderr, "Could not arm FPGA load failure injection\n");
            status = BLADERF_ERR_UNEXPECTED;
            goto error;
        }
#else
        fprintf(stderr, "This test binary lacks FPGA load failure injection\n");
        status = BLADERF_ERR_UNEXPECTED;
        goto error;
#endif
        status = bladerf_load_fpga(dev, fpga_file);
#ifdef BLADERF_ENABLE_TEST_FPGA_LOAD_FAILURE_INJECTION
        int disarm_status = failed_backend_reload_only
            ? set_fpga_backend_failure_injection(0)
            : set_fpga_load_failure_injection(0, 0);
        if (disarm_status != 0) {
            fprintf(stderr, "Could not disarm FPGA load failure injection\n");
            status = BLADERF_ERR_UNEXPECTED;
            goto error;
        }
#endif
        int expected_failure = failed_backend_reload_only
            ? BLADERF_ERR_IO : BLADERF_ERR_UNEXPECTED;
        if (status != expected_failure) {
            fprintf(stderr, "Expected injected reload failure %d, got %d\n",
                    expected_failure, status);
            goto error;
        }
        status = 0;
        if (report_unconfigured || failed_backend_reload_only) {
            printf("Checking RF tuning is refused until a fresh FPGA load...\n");
            status = bladerf_set_frequency(
                dev, BLADERF_CHANNEL_RX(0), 1835000000ULL);
            if (status == 0) {
                fprintf(stderr, "RF frequency update succeeded while FPGA "
                        "state was fail-closed\n");
                status = BLADERF_ERR_UNEXPECTED;
                goto error;
            }
            status = 0;
        } else {
            printf("Checking RF tuning remains available after recovery...\n");
            CHECK_STATUS(bladerf_set_frequency(
                dev, BLADERF_CHANNEL_RX(0), 1835000000ULL));
        }
    }

    printf("Reloading the FPGA image...\n");
    CHECK_STATUS(bladerf_load_fpga(dev, fpga_file));

    if (failed_backend_reload_only) {
        printf("Verifying same-handle event-driven RX after backend recovery...\n");
        CHECK_STATUS(verify_recovered_rx_event_chain(dev));
    }

    if (reload_only) {
        if (failed_reload_only || failed_backend_reload_only) {
            printf("Passed failed-reload fail-closed/recovery test!\n");
        } else {
            printf("Passed same-handle FPGA reload lifecycle test!\n");
        }
        goto error;
    }

    printf("Setting tuning mode to FPGA...\n");
    CHECK_STATUS(bladerf_set_tuning_mode(dev, BLADERF_TUNING_MODE_FPGA));

    printf("Setting sample rate again to 10e6...\n");
    CHECK_STATUS(bladerf_set_sample_rate(dev, BLADERF_MODULE_RX, 10e6, NULL));

    printf("Passed!\n");
error:
    if (dev) {
        bladerf_close(dev);
    }
    return status;
}
