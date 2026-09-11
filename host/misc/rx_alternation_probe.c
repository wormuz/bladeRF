/* Why RX captures alternate pass/fail across repeated runs.
 *
 * Running bladeRF-cli in a loop gives 0, 200000, 0, 200000 ... and the host
 * command sequence is identical in the passing and failing logs, so the
 * difference is device state carried between processes rather than anything
 * the host decides. This isolates the one thing a separate process cannot
 * control: whether libusb resets the port when the device is opened.
 *
 * Each iteration opens, captures, closes -- the same shape as a fresh
 * bladeRF-cli run, but with the port reset under our control.
 *
 *   cc -o rx_alternation_probe rx_alternation_probe.c -lbladeRF
 *   ./rx_alternation_probe 6 1   # reset on open enabled
 *   ./rx_alternation_probe 6 0   # disabled
 */

#include <libbladeRF.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NUM_SAMPLES 50000u
#define NUM_BUFFERS 16u
#define BUFFER_SIZE 8192u
#define NUM_XFERS   8u
#define TIMEOUT_MS  5000u

/* Returns samples received, or -1 on any failure before the transfer. */
static int one_capture(void)
{
    struct bladerf *dev = NULL;
    int16_t *buf        = NULL;
    int status;
    int received = -1;

    status = bladerf_open(&dev, NULL);
    if (status != 0) {
        fprintf(stderr, "  open: %s\n", bladerf_strerror(status));
        return -1;
    }

    buf = malloc(NUM_SAMPLES * 2 * sizeof(int16_t));
    if (buf == NULL) {
        goto out;
    }

    status = bladerf_set_frequency(dev, BLADERF_CHANNEL_RX(0), 925000000);
    if (status != 0) {
        fprintf(stderr, "  set_frequency: %s\n", bladerf_strerror(status));
        goto out;
    }

    status = bladerf_set_sample_rate(dev, BLADERF_CHANNEL_RX(0), 61440000, NULL);
    if (status != 0) {
        fprintf(stderr, "  set_sample_rate: %s\n", bladerf_strerror(status));
        goto out;
    }

    status = bladerf_sync_config(dev, BLADERF_RX_X1, BLADERF_FORMAT_SC16_Q11,
                                 NUM_BUFFERS, BUFFER_SIZE, NUM_XFERS,
                                 TIMEOUT_MS);
    if (status != 0) {
        fprintf(stderr, "  sync_config: %s\n", bladerf_strerror(status));
        goto out;
    }

    status = bladerf_enable_module(dev, BLADERF_CHANNEL_RX(0), true);
    if (status != 0) {
        fprintf(stderr, "  enable: %s\n", bladerf_strerror(status));
        goto out;
    }

    status = bladerf_sync_rx(dev, buf, NUM_SAMPLES, NULL, TIMEOUT_MS);
    received = (status == 0) ? (int)NUM_SAMPLES : 0;
    if (status != 0) {
        fprintf(stderr, "  sync_rx: %s\n", bladerf_strerror(status));
    }

    bladerf_enable_module(dev, BLADERF_CHANNEL_RX(0), false);

out:
    free(buf);
    bladerf_close(dev);
    return received;
}

int main(int argc, char *argv[])
{
    unsigned iterations = (argc > 1) ? (unsigned)atoi(argv[1]) : 6;
    bool reset_on_open  = (argc > 2) ? (atoi(argv[2]) != 0) : true;
    unsigned passed = 0, failed = 0;
    unsigned i;

    bladerf_set_usb_reset_on_open(reset_on_open);
    printf("reset_on_open=%d, %u iterations\n", (int)reset_on_open, iterations);

    for (i = 0; i < iterations; i++) {
        int got = one_capture();

        printf("  iter %u: %d samples\n", i + 1, got);
        if (got == (int)NUM_SAMPLES) {
            passed++;
        } else {
            failed++;
        }
    }

    printf("passed=%u failed=%u\n", passed, failed);
    return (failed == 0) ? 0 : 1;
}
