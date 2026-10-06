/* Live xA4 qualification of NIOS pkt_retune2 scheduled RX transitions.
 * This is intentionally separate from host-only tests: it tunes the RFIC and
 * requires an FPGA image with scheduled-retune support. */
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <libbladeRF.h>

#define RX_CHANNEL BLADERF_CHANNEL_RX(0)
#define RX_PROFILE_SETUP_REG 0x25a
#define RX_FASTLOCK_PROFILE_MASK 0xe0
#define RX_FASTLOCK_MODE_ENABLE 0x01
#define RX_BLOCK_SAMPLES 8192
#define RX_SAMPLE_RATE 3840000
#define RX_BANDWIDTH 1500000

static int check_status(int status, const char *operation)
{
    if (status == 0) {
        return 0;
    }

    fprintf(stderr, "%s: %s (%d)\n", operation,
            bladerf_strerror(status), status);
    return status;
}

int main(int argc, char **argv)
{
    static const bladerf_frequency frequency[2] = {
        UINT64_C(1835000000), UINT64_C(1835400000)
    };
    struct bladerf *dev = NULL;
    struct bladerf_quick_tune quick_tune[2];
    struct bladerf_metadata metadata = { .flags = BLADERF_META_FLAG_RX_NOW };
    struct bladerf_version fpga_version;
    int16_t *samples = NULL;
    unsigned int actual_rate = 0;
    unsigned int actual_bandwidth = 0;
    unsigned int trials = argc > 1 ? (unsigned int)strtoul(argv[1], NULL, 10) : 100;
    unsigned int i;
    int status = 1;

    if (trials == 0) {
        fputs("trial count must be positive\n", stderr);
        return 2;
    }

    samples = calloc(RX_BLOCK_SAMPLES * 2, sizeof(*samples));
    if (samples == NULL) {
        perror("calloc");
        return 2;
    }

    if (check_status(bladerf_open(&dev, NULL), "open")) {
        goto done;
    }
    bladerf_fpga_version(dev, &fpga_version);
    printf("FPGA %u.%u.%u; testing %u scheduled RX transitions\n",
           fpga_version.major, fpga_version.minor, fpga_version.patch, trials);

    if (check_status(bladerf_set_sample_rate(dev, RX_CHANNEL, RX_SAMPLE_RATE,
                                             &actual_rate), "sample rate") ||
        check_status(bladerf_set_bandwidth(dev, RX_CHANNEL, RX_BANDWIDTH,
                                           &actual_bandwidth), "bandwidth") ||
        check_status(bladerf_set_frequency(dev, RX_CHANNEL, frequency[0]),
                     "tune profile A") ||
        check_status(bladerf_get_quick_tune(dev, RX_CHANNEL, &quick_tune[0]),
                     "read profile A") ||
        check_status(bladerf_set_frequency(dev, RX_CHANNEL, frequency[1]),
                     "tune profile B") ||
        check_status(bladerf_get_quick_tune(dev, RX_CHANNEL, &quick_tune[1]),
                     "read profile B") ||
        check_status(bladerf_set_frequency(dev, RX_CHANNEL, frequency[0]),
                     "restore profile A") ||
        check_status(bladerf_sync_config(dev, BLADERF_RX_X1,
                                         BLADERF_FORMAT_SC16_Q11_META,
                                         32, RX_BLOCK_SAMPLES, 16, 1000),
                     "configure META RX") ||
        check_status(bladerf_enable_module(dev, RX_CHANNEL, true),
                     "enable RX") ||
        check_status(bladerf_sync_rx(dev, samples, RX_BLOCK_SAMPLES,
                                     &metadata, 1000), "start RX")) {
        goto done;
    }

    for (i = 0; i < trials; i++) {
        const unsigned int target_index = (i + 1) & 1u;
        uint64_t now;
        uint64_t target_timestamp;
        uint64_t end_timestamp;
        uint8_t fastlock_setup;

        if (check_status(bladerf_get_timestamp(dev, BLADERF_RX, &now),
                         "read RX timestamp")) {
            goto done;
        }

        /* Ten milliseconds gives NIOS time to load/arm the queue entry while
         * still checking that recall is gated by the requested sample time. */
        target_timestamp = now + RX_SAMPLE_RATE / 100;
        if (check_status(bladerf_schedule_retune(
                dev, RX_CHANNEL, target_timestamp, frequency[target_index],
                &quick_tune[target_index]), "schedule RX retune")) {
            goto done;
        }

        /* Read well beyond the target boundary so this tests timer delivery
         * and async profile activation, not a guessed analog settle interval. */
        end_timestamp = target_timestamp + RX_SAMPLE_RATE / 10;
        do {
            metadata = (struct bladerf_metadata){
                .flags = BLADERF_META_FLAG_RX_NOW
            };
            if (check_status(bladerf_sync_rx(dev, samples, RX_BLOCK_SAMPLES,
                                             &metadata, 1000), "read RX META")) {
                goto done;
            }
            if (metadata.flags & BLADERF_META_STATUS_OVERRUN) {
                fprintf(stderr, "RX overrun at trial %u, timestamp=%" PRIu64
                        "\n", i, metadata.timestamp);
                goto done;
            }
        } while (metadata.timestamp < end_timestamp);

        if (check_status(bladerf_get_rfic_register(
                dev, RX_PROFILE_SETUP_REG, &fastlock_setup),
                         "read RX fastlock setup")) {
            goto done;
        }

        if (((fastlock_setup & RX_FASTLOCK_PROFILE_MASK) >> 5) !=
                quick_tune[target_index].rffe_profile ||
            (fastlock_setup & RX_FASTLOCK_MODE_ENABLE) == 0) {
            fprintf(stderr, "trial %u: expected active RFFE profile %u; "
                    "RX_FAST_LOCK_SETUP=0x%02x\n", i,
                    quick_tune[target_index].rffe_profile, fastlock_setup);
            goto done;
        }
    }

    printf("PASS: %u/%u scheduled RX timer boundaries selected the expected "
           "fastlock profile; no META overruns (rate=%u, bandwidth=%u)\n",
           trials, trials, actual_rate, actual_bandwidth);
    status = 0;

done:
    if (dev != NULL) {
        (void)bladerf_cancel_scheduled_retunes(dev, RX_CHANNEL);
        (void)bladerf_enable_module(dev, RX_CHANNEL, false);
        bladerf_close(dev);
    }
    free(samples);
    return status;
}
