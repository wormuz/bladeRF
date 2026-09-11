/* How far a scheduled TX burst release lands from its requested timestamp,
 * observed on the same device: TX leaks into RX at the same frequency, so we
 * do not need a second radio or antenna to see the envelope of the burst.
 *
 * This does not measure waveform fidelity -- only when the burst actually
 * appears in the RX stream relative to the FPGA timestamp we asked TX to
 * fire at. That is the number needed to know how much lead time a caller
 * must give bladerf_sync_tx() metadata scheduling to land reliably.
 *
 *   cc -o tx_timestamp_probe tx_timestamp_probe.c -lbladeRF
 *   ./tx_timestamp_probe [lead_ms]     # default lead 20 ms
 */

#include <libbladeRF.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FREQ_HZ        925000000u
#define SAMPLERATE_HZ  10000000u
#define BANDWIDTH_HZ   5000000u
#define RX_GAIN_DB     5
#define TX_GAIN_DB     (-20)

#define NUM_BUFFERS 32u
#define BUFFER_SIZE 8192u
#define NUM_XFERS   8u
#define TIMEOUT_MS  2000u

#define BURST_SAMPLES 8192u
#define TONE_PERIOD   32u
#define TONE_AMPL     1500

#define BLOCK_SAMPLES     256u
#define POWER_RATIO_THRESH 8.0

#define RX_SEARCH_MARGIN_MS 60u

static int16_t *make_tone(unsigned int n)
{
    int16_t *buf = malloc((size_t)n * 2u * sizeof(int16_t));
    unsigned int i;

    if (buf == NULL) {
        return NULL;
    }

    for (i = 0; i < n; i++) {
        double phase = 2.0 * M_PI * (double)(i % TONE_PERIOD) / (double)TONE_PERIOD;
        buf[2 * i]     = (int16_t)lround(TONE_AMPL * cos(phase));
        buf[2 * i + 1] = (int16_t)lround(TONE_AMPL * sin(phase));
    }

    return buf;
}

/* Mean |I|+|Q| over one block of BLOCK_SAMPLES, starting at sample offset
 * `block` within `buf` (which holds `n` samples total). */
static double block_power(const int16_t *buf, unsigned int block)
{
    unsigned int base = block * BLOCK_SAMPLES;
    unsigned int i;
    long sum = 0;

    for (i = 0; i < BLOCK_SAMPLES; i++) {
        int32_t iv = buf[2 * (base + i)];
        int32_t qv = buf[2 * (base + i) + 1];
        sum += labs(iv) + labs(qv);
    }

    return (double)sum / (double)BLOCK_SAMPLES;
}

static int cmp_double(const void *a, const void *b)
{
    double da = *(const double *)a;
    double db = *(const double *)b;
    return (da > db) - (da < db);
}

int main(int argc, char *argv[])
{
    unsigned int lead_ms = (argc > 1) ? (unsigned int)atoi(argv[1]) : 20u;
    struct bladerf *dev  = NULL;
    int16_t *tx_buf       = NULL;
    int16_t *rx_buf        = NULL;
    unsigned int n_blocks_per_chunk = BUFFER_SIZE / BLOCK_SAMPLES;
    struct bladerf_metadata tx_meta;
    struct bladerf_metadata rx_meta;
    bladerf_timestamp ts_rx0 = 0, ts_tx0 = 0;
    bladerf_timestamp lead_samples;
    bladerf_timestamp search_limit;
    bool overrun_seen  = false;
    bool burst_found    = false;
    double first_chunk_median = 0.0;
    int status;
    int ret = 1;

    tx_buf = make_tone(BURST_SAMPLES);
    rx_buf  = malloc((size_t)BUFFER_SIZE * 2u * sizeof(int16_t));
    if (tx_buf == NULL || rx_buf == NULL) {
        fprintf(stderr, "alloc failed\n");
        goto out_free;
    }

    status = bladerf_open(&dev, NULL);
    if (status != 0) {
        fprintf(stderr, "open: %s\n", bladerf_strerror(status));
        goto out_free;
    }

    status = bladerf_set_frequency(dev, BLADERF_CHANNEL_RX(0), FREQ_HZ);
    if (status != 0) {
        fprintf(stderr, "set_frequency rx: %s\n", bladerf_strerror(status));
        goto out_close;
    }
    status = bladerf_set_frequency(dev, BLADERF_CHANNEL_TX(0), FREQ_HZ);
    if (status != 0) {
        fprintf(stderr, "set_frequency tx: %s\n", bladerf_strerror(status));
        goto out_close;
    }

    status = bladerf_set_sample_rate(dev, BLADERF_CHANNEL_RX(0), SAMPLERATE_HZ, NULL);
    if (status != 0) {
        fprintf(stderr, "set_sample_rate rx: %s\n", bladerf_strerror(status));
        goto out_close;
    }
    status = bladerf_set_sample_rate(dev, BLADERF_CHANNEL_TX(0), SAMPLERATE_HZ, NULL);
    if (status != 0) {
        fprintf(stderr, "set_sample_rate tx: %s\n", bladerf_strerror(status));
        goto out_close;
    }

    status = bladerf_set_bandwidth(dev, BLADERF_CHANNEL_RX(0), BANDWIDTH_HZ, NULL);
    if (status != 0) {
        fprintf(stderr, "set_bandwidth rx: %s\n", bladerf_strerror(status));
        goto out_close;
    }
    status = bladerf_set_bandwidth(dev, BLADERF_CHANNEL_TX(0), BANDWIDTH_HZ, NULL);
    if (status != 0) {
        fprintf(stderr, "set_bandwidth tx: %s\n", bladerf_strerror(status));
        goto out_close;
    }

    status = bladerf_set_gain_mode(dev, BLADERF_CHANNEL_RX(0), BLADERF_GAIN_MGC);
    if (status != 0) {
        fprintf(stderr, "set_gain_mode: %s (continuing)\n", bladerf_strerror(status));
    }
    status = bladerf_set_gain(dev, BLADERF_CHANNEL_RX(0), RX_GAIN_DB);
    if (status != 0) {
        fprintf(stderr, "set_gain rx: %s (continuing)\n", bladerf_strerror(status));
    }
    status = bladerf_set_gain(dev, BLADERF_CHANNEL_TX(0), TX_GAIN_DB);
    if (status != 0) {
        fprintf(stderr, "set_gain tx: %s (continuing)\n", bladerf_strerror(status));
    }

    status = bladerf_sync_config(dev, BLADERF_RX_X1, BLADERF_FORMAT_SC16_Q11_META,
                                 NUM_BUFFERS, BUFFER_SIZE, NUM_XFERS, TIMEOUT_MS);
    if (status != 0) {
        fprintf(stderr, "sync_config rx: %s\n", bladerf_strerror(status));
        goto out_close;
    }
    status = bladerf_sync_config(dev, BLADERF_TX_X1, BLADERF_FORMAT_SC16_Q11_META,
                                 NUM_BUFFERS, BUFFER_SIZE, NUM_XFERS, TIMEOUT_MS);
    if (status != 0) {
        fprintf(stderr, "sync_config tx: %s\n", bladerf_strerror(status));
        goto out_close;
    }

    status = bladerf_enable_module(dev, BLADERF_CHANNEL_RX(0), true);
    if (status != 0) {
        fprintf(stderr, "enable rx: %s\n", bladerf_strerror(status));
        goto out_close;
    }
    status = bladerf_enable_module(dev, BLADERF_CHANNEL_TX(0), true);
    if (status != 0) {
        fprintf(stderr, "enable tx: %s\n", bladerf_strerror(status));
        goto out_disable_rx;
    }

    status = bladerf_get_timestamp(dev, BLADERF_RX, &ts_rx0);
    if (status != 0) {
        fprintf(stderr, "get_timestamp rx: %s\n", bladerf_strerror(status));
        goto out_disable_tx;
    }
    status = bladerf_get_timestamp(dev, BLADERF_TX, &ts_tx0);
    if (status != 0) {
        fprintf(stderr, "get_timestamp tx: %s\n", bladerf_strerror(status));
        goto out_disable_tx;
    }
    printf("ts_rx0=%llu\n", (unsigned long long)ts_rx0);
    printf("ts_tx0=%llu\n", (unsigned long long)ts_tx0);

    lead_samples = (bladerf_timestamp)lead_ms * (SAMPLERATE_HZ / 1000u);

    memset(&tx_meta, 0, sizeof(tx_meta));
    tx_meta.timestamp = ts_tx0 + lead_samples;
    tx_meta.flags = BLADERF_META_FLAG_TX_BURST_START | BLADERF_META_FLAG_TX_BURST_END;

    printf("lead_ms=%u\n", lead_ms);
    printf("tx_scheduled_timestamp=%llu\n", (unsigned long long)tx_meta.timestamp);

    status = bladerf_sync_tx(dev, tx_buf, BURST_SAMPLES, &tx_meta, 5000);
    if (status != 0) {
        fprintf(stderr, "sync_tx: %s\n", bladerf_strerror(status));
        goto out_disable_tx;
    }
    printf("tx_status=0x%x\n", tx_meta.status);

    search_limit = tx_meta.timestamp + (bladerf_timestamp)RX_SEARCH_MARGIN_MS * (SAMPLERATE_HZ / 1000u);

    memset(&rx_meta, 0, sizeof(rx_meta));
    rx_meta.flags = BLADERF_META_FLAG_RX_NOW;

    {
        double max_power_seen = 0.0;
        double median_seen     = 0.0;
        bool first_chunk        = true;

        while (!burst_found) {
            status = bladerf_sync_rx(dev, rx_buf, BUFFER_SIZE, &rx_meta, TIMEOUT_MS);
            if (status != 0) {
                fprintf(stderr, "sync_rx: %s\n", bladerf_strerror(status));
                break;
            }

            if (rx_meta.status & BLADERF_META_STATUS_OVERRUN) {
                overrun_seen = true;
            }
            if (rx_meta.status & BLADERF_META_STATUS_UNDERRUN) {
                printf("rx_chunk_underrun=1\n");
            }

            if (first_chunk) {
                double powers[BUFFER_SIZE / BLOCK_SAMPLES];
                unsigned int b;

                for (b = 0; b < n_blocks_per_chunk; b++) {
                    powers[b] = block_power(rx_buf, b);
                }
                qsort(powers, n_blocks_per_chunk, sizeof(double), cmp_double);
                first_chunk_median = powers[n_blocks_per_chunk / 2];
                median_seen = first_chunk_median;
                first_chunk = false;
                printf("first_chunk_median_power=%.2f\n", first_chunk_median);
            }

            {
                unsigned int b;
                for (b = 0; b < n_blocks_per_chunk; b++) {
                    double p = block_power(rx_buf, b);
                    if (p > max_power_seen) {
                        max_power_seen = p;
                    }
                    if (median_seen > 0.0 && p > POWER_RATIO_THRESH * median_seen) {
                        bladerf_timestamp burst_ts = rx_meta.timestamp + b * BLOCK_SAMPLES;
                        long long diff_samples = (long long)burst_ts - (long long)tx_meta.timestamp;
                        double diff_us = (double)diff_samples * 1e6 / (double)SAMPLERATE_HZ;

                        printf("rx_burst_start_timestamp=%llu\n", (unsigned long long)burst_ts);
                        printf("diff_samples=%lld\n", diff_samples);
                        printf("diff_us=%.2f\n", diff_us);
                        burst_found = true;
                        break;
                    }
                }
            }

            if (!burst_found && rx_meta.timestamp > search_limit) {
                printf("no burst detected\n");
                printf("max_block_power=%.2f\n", max_power_seen);
                printf("median_power=%.2f\n", median_seen);
                break;
            }
        }
    }

    printf("overrun_seen=%d\n", (int)overrun_seen);
    ret = burst_found ? 0 : 1;

out_disable_tx:
    bladerf_enable_module(dev, BLADERF_CHANNEL_TX(0), false);
out_disable_rx:
    bladerf_enable_module(dev, BLADERF_CHANNEL_RX(0), false);
out_close:
    bladerf_close(dev);
out_free:
    free(tx_buf);
    free(rx_buf);
    return ret;
}
