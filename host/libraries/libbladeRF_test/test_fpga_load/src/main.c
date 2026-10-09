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

#ifdef BLADERF_ENABLE_TEST_FPGA_LOAD_FAILURE_INJECTION
static int set_fpga_load_failure_injection(int enabled)
{
#ifdef _WIN32
    return _putenv_s("BLADERF_TEST_FAIL_FPGA_RELOAD_ONCE",
                     enabled ? "1" : "");
#else
    return enabled
        ? setenv("BLADERF_TEST_FAIL_FPGA_RELOAD_ONCE", "1", 1)
        : unsetenv("BLADERF_TEST_FAIL_FPGA_RELOAD_ONCE");
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
            {0, 0, 0, 0 }
        };

        c = getopt_long(argc, argv, "d:f:vrF", long_options, &option_index);
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

    if (failed_reload_only) {
        printf("Injecting one FPGA reload failure, then checking recovery...\n");
#ifdef BLADERF_ENABLE_TEST_FPGA_LOAD_FAILURE_INJECTION
        if (set_fpga_load_failure_injection(1) != 0) {
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
        if (set_fpga_load_failure_injection(0) != 0) {
            fprintf(stderr, "Could not disarm FPGA load failure injection\n");
            status = BLADERF_ERR_UNEXPECTED;
            goto error;
        }
#endif
        if (status != BLADERF_ERR_UNEXPECTED) {
            fprintf(stderr, "Expected injected BLADERF_ERR_UNEXPECTED, got %d\n",
                    status);
            goto error;
        }
        status = 0;
    }

    printf("Reloading the FPGA image...\n");
    CHECK_STATUS(bladerf_load_fpga(dev, fpga_file));

    if (reload_only) {
        if (failed_reload_only) {
            printf("Passed failed-reload recovery and subsequent reload test!\n");
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
