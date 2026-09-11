/* How far a scheduled TX burst release lands from its requested timestamp,
 * observed on the same device: TX leaks into RX at the same frequency, so we
 * do not need a second radio or antenna to see the envelope of the burst.
 *
 * This does not measure waveform fidelity -- only when the burst actually
 * appears in the RX stream relative to the FPGA timestamp we asked TX to
 * fire at. That is the number needed to know how much lead time a caller
 * must give bladerf_sync_tx() metadata scheduling to land reliably.
 *
 *   cc -o tx_timestamp_probe tx_timestamp_probe.c -lbladeRF -lm
 *   ./tx_timestamp_probe [lead_ms] [lb|air] [channel 0|1] [tx_gain_db] [rx_gain_db]
 *   e.g.  ./tx_timestamp_probe 20 air 1    # TX2 -> 30 dB pad -> RX2
 *
 * channel 1 uses BLADERF_RX_X2 / BLADERF_TX_X2 (MIMO) layouts: per
 * libbladeRF.h (BLADERF_FORMAT_SC16_Q11 docs), samples are interleaved
 * per channel (ch0 I/Q, ch1 I/Q, ch0 I/Q, ...) and num_samples passed to
 * bladerf_sync_rx()/bladerf_sync_tx() is a PER-CHANNEL count (buffer_size_min
 * = 2 * num_samples * num_channels * sizeof(int16_t)). Channel 0 is left
 * silent on TX and ignored on RX detection; only channel 1 carries the tone
 * and is analyzed. Per doc/examples/sync_rx_meta.c, X2 layout requires
 * enabling BOTH channel 0 and channel 1 modules (not just channel 1).
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
#define DEFAULT_RX_GAIN_DB     30
#define DEFAULT_TX_GAIN_DB     20

#define NUM_BUFFERS 32u
#define BUFFER_SIZE 8192u
#define NUM_XFERS   8u
#define TIMEOUT_MS  2000u

#define BURST_SAMPLES 8192u
#define TONE_PERIOD   32u
#define TONE_AMPL     1500

#define BLOCK_SAMPLES     256u
#define POWER_RATIO_THRESH 8.0
#define ABS_FLOOR         200.0

#define RX_SEARCH_MARGIN_MS 60u
#define RX_SEARCH_MARGIN_PRE_MS 5u

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

/* Mean |I|+|Q| over one block of BLOCK_SAMPLES, starting at per-channel
 * sample offset `block` within `buf`. `samples_per_frame` is the number of
 * int16_t pairs (I,Q) per interleaved sample frame across all channels (1
 * for X1, 2 for X2); `pair_offset` selects which channel's I/Q pair within
 * the frame (0 for ch0, 1 for ch1). */
static double block_power(const int16_t *buf, unsigned int block,
                           unsigned int samples_per_frame,
                           unsigned int pair_offset)
{
    unsigned int base = block * BLOCK_SAMPLES;
    unsigned int i;
    long sum = 0;

    for (i = 0; i < BLOCK_SAMPLES; i++) {
        unsigned int frame = (base + i) * samples_per_frame + pair_offset;
        int32_t iv = buf[2 * frame];
        int32_t qv = buf[2 * frame + 1];
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
    /* Third argument: channel index, 0 = RX1/TX1 (X1/SISO layout), 1 =
     * RX2/TX2 (X2/MIMO layout, cabled loop through an attenuator lives on
     * channel 2 on the bench). */
    unsigned int ch = (argc > 3) ? (unsigned int)atoi(argv[3]) : 0u;
    bool mimo = (ch == 1u);
    unsigned int samples_per_frame = mimo ? 2u : 1u;
    unsigned int tx_gain_db = (argc > 4) ? (unsigned int)atoi(argv[4]) : DEFAULT_TX_GAIN_DB;
    unsigned int rx_gain_db = (argc > 5) ? (unsigned int)atoi(argv[5]) : DEFAULT_RX_GAIN_DB;
    struct bladerf *dev  = NULL;
    int16_t *tone_buf     = NULL;
    int16_t *tx_buf       = NULL;
    int16_t *rx_buf        = NULL;
    unsigned int n_blocks_per_chunk = BUFFER_SIZE / BLOCK_SAMPLES;
    struct bladerf_metadata tx_meta;
    struct bladerf_metadata rx_meta;
    bladerf_timestamp ts_rx0 = 0, ts_tx0 = 0;
    bladerf_timestamp lead_samples;
    bladerf_timestamp search_limit;
    bladerf_timestamp search_start_ts;
    bool overrun_seen  = false;
    bool burst_found    = false;
    double first_chunk_median = 0.0;
    int status;
    int ret = 1;

    tone_buf = make_tone(BURST_SAMPLES);
    /* tx_buf/rx_buf hold `samples_per_frame` interleaved channels; buffer
     * size follows libbladeRF.h buffer_size_min = 2 * num_samples *
     * num_channels * sizeof(int16_t), i.e. BUFFER_SIZE/BURST_SAMPLES is a
     * PER-CHANNEL sample count multiplied here by samples_per_frame. */
    tx_buf = malloc((size_t)BURST_SAMPLES * samples_per_frame * 2u * sizeof(int16_t));
    rx_buf  = malloc((size_t)BUFFER_SIZE * samples_per_frame * 2u * sizeof(int16_t));
    if (tone_buf == NULL || tx_buf == NULL || rx_buf == NULL) {
        fprintf(stderr, "alloc failed\n");
        goto out_free;
    }

    if (mimo) {
        unsigned int i;
        memset(tx_buf, 0, (size_t)BURST_SAMPLES * samples_per_frame * 2u * sizeof(int16_t));
        for (i = 0; i < BURST_SAMPLES; i++) {
            tx_buf[4 * i + 2] = tone_buf[2 * i];
            tx_buf[4 * i + 3] = tone_buf[2 * i + 1];
        }
    } else {
        memcpy(tx_buf, tone_buf, (size_t)BURST_SAMPLES * 2u * sizeof(int16_t));
    }

    status = bladerf_open(&dev, NULL);
    if (status != 0) {
        fprintf(stderr, "open: %s\n", bladerf_strerror(status));
        goto out_free;
    }

    /* Second argument "lb": route TX back to RX inside the AD9361 (BIST
     * loopback). Over-the-air leakage on one board was too weak to find
     * the burst (max block power ~2x the median); the digital loopback
     * still exercises the FPGA TX release path, which is what is being
     * timed here. */
    if (argc > 2 && strcmp(argv[2], "lb") == 0) {
        status = bladerf_set_loopback(dev, BLADERF_LB_RFIC_BIST);
        printf("loopback=rfic_bist status=%d\n", status);
    }

    status = bladerf_set_frequency(dev, BLADERF_CHANNEL_RX(ch), FREQ_HZ);
    if (status != 0) {
        fprintf(stderr, "set_frequency rx: %s\n", bladerf_strerror(status));
        goto out_close;
    }
    status = bladerf_set_frequency(dev, BLADERF_CHANNEL_TX(ch), FREQ_HZ);
    if (status != 0) {
        fprintf(stderr, "set_frequency tx: %s\n", bladerf_strerror(status));
        goto out_close;
    }

    status = bladerf_set_sample_rate(dev, BLADERF_CHANNEL_RX(ch), SAMPLERATE_HZ, NULL);
    if (status != 0) {
        fprintf(stderr, "set_sample_rate rx: %s\n", bladerf_strerror(status));
        goto out_close;
    }
    status = bladerf_set_sample_rate(dev, BLADERF_CHANNEL_TX(ch), SAMPLERATE_HZ, NULL);
    if (status != 0) {
        fprintf(stderr, "set_sample_rate tx: %s\n", bladerf_strerror(status));
        goto out_close;
    }

    status = bladerf_set_bandwidth(dev, BLADERF_CHANNEL_RX(ch), BANDWIDTH_HZ, NULL);
    if (status != 0) {
        fprintf(stderr, "set_bandwidth rx: %s\n", bladerf_strerror(status));
        goto out_close;
    }
    status = bladerf_set_bandwidth(dev, BLADERF_CHANNEL_TX(ch), BANDWIDTH_HZ, NULL);
    if (status != 0) {
        fprintf(stderr, "set_bandwidth tx: %s\n", bladerf_strerror(status));
        goto out_close;
    }

    status = bladerf_set_gain_mode(dev, BLADERF_CHANNEL_RX(ch), BLADERF_GAIN_MGC);
    if (status != 0) {
        fprintf(stderr, "set_gain_mode: %s (continuing)\n", bladerf_strerror(status));
    }
    status = bladerf_set_gain(dev, BLADERF_CHANNEL_RX(ch), (int)rx_gain_db);
    if (status != 0) {
        fprintf(stderr, "set_gain rx: %s (continuing)\n", bladerf_strerror(status));
    }
    status = bladerf_set_gain(dev, BLADERF_CHANNEL_TX(ch), (int)tx_gain_db);
    if (status != 0) {
        fprintf(stderr, "set_gain tx: %s (continuing)\n", bladerf_strerror(status));
    }
    printf("tx_gain_db=%u\n", tx_gain_db);
    printf("rx_gain_db=%u\n", rx_gain_db);

    if (mimo) {
        /* Channel 0 shares the RFIC direction with channel 1 under X2; it
         * must be configured and enabled too (doc/examples/sync_rx_meta.c
         * enables both RX(0) and RX(1) before an X2 sync_rx call). Channel
         * 0 stays silent on TX and unanalyzed on RX. */
        status = bladerf_set_frequency(dev, BLADERF_CHANNEL_RX(0), FREQ_HZ);
        if (status != 0) {
            fprintf(stderr, "set_frequency rx0: %s\n", bladerf_strerror(status));
            goto out_close;
        }
        status = bladerf_set_frequency(dev, BLADERF_CHANNEL_TX(0), FREQ_HZ);
        if (status != 0) {
            fprintf(stderr, "set_frequency tx0: %s\n", bladerf_strerror(status));
            goto out_close;
        }
        status = bladerf_set_sample_rate(dev, BLADERF_CHANNEL_RX(0), SAMPLERATE_HZ, NULL);
        if (status != 0) {
            fprintf(stderr, "set_sample_rate rx0: %s\n", bladerf_strerror(status));
            goto out_close;
        }
        status = bladerf_set_sample_rate(dev, BLADERF_CHANNEL_TX(0), SAMPLERATE_HZ, NULL);
        if (status != 0) {
            fprintf(stderr, "set_sample_rate tx0: %s\n", bladerf_strerror(status));
            goto out_close;
        }
        status = bladerf_set_bandwidth(dev, BLADERF_CHANNEL_RX(0), BANDWIDTH_HZ, NULL);
        if (status != 0) {
            fprintf(stderr, "set_bandwidth rx0: %s\n", bladerf_strerror(status));
            goto out_close;
        }
        status = bladerf_set_bandwidth(dev, BLADERF_CHANNEL_TX(0), BANDWIDTH_HZ, NULL);
        if (status != 0) {
            fprintf(stderr, "set_bandwidth tx0: %s\n", bladerf_strerror(status));
            goto out_close;
        }
        status = bladerf_set_gain_mode(dev, BLADERF_CHANNEL_RX(0), BLADERF_GAIN_MGC);
        if (status != 0) {
            fprintf(stderr, "set_gain_mode rx0: %s (continuing)\n", bladerf_strerror(status));
        }
        status = bladerf_set_gain(dev, BLADERF_CHANNEL_RX(0), (int)rx_gain_db);
        if (status != 0) {
            fprintf(stderr, "set_gain rx0: %s (continuing)\n", bladerf_strerror(status));
        }
        status = bladerf_set_gain(dev, BLADERF_CHANNEL_TX(0), (int)tx_gain_db);
        if (status != 0) {
            fprintf(stderr, "set_gain tx0: %s (continuing)\n", bladerf_strerror(status));
        }
    }

    status = bladerf_sync_config(dev, mimo ? BLADERF_RX_X2 : BLADERF_RX_X1,
                                 BLADERF_FORMAT_SC16_Q11_META,
                                 NUM_BUFFERS, BUFFER_SIZE, NUM_XFERS, TIMEOUT_MS);
    if (status != 0) {
        fprintf(stderr, "sync_config rx: %s\n", bladerf_strerror(status));
        goto out_close;
    }
    status = bladerf_sync_config(dev, mimo ? BLADERF_TX_X2 : BLADERF_TX_X1,
                                 BLADERF_FORMAT_SC16_Q11_META,
                                 NUM_BUFFERS, BUFFER_SIZE, NUM_XFERS, TIMEOUT_MS);
    if (status != 0) {
        fprintf(stderr, "sync_config tx: %s\n", bladerf_strerror(status));
        goto out_close;
    }

    if (mimo) {
        status = bladerf_enable_module(dev, BLADERF_CHANNEL_RX(0), true);
        if (status != 0) {
            fprintf(stderr, "enable rx0: %s\n", bladerf_strerror(status));
            goto out_close;
        }
        status = bladerf_enable_module(dev, BLADERF_CHANNEL_TX(0), true);
        if (status != 0) {
            fprintf(stderr, "enable tx0: %s\n", bladerf_strerror(status));
            goto out_disable_rx;
        }
    }

    status = bladerf_enable_module(dev, BLADERF_CHANNEL_RX(ch), true);
    if (status != 0) {
        fprintf(stderr, "enable rx: %s\n", bladerf_strerror(status));
        goto out_close;
    }
    status = bladerf_enable_module(dev, BLADERF_CHANNEL_TX(ch), true);
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

    /* Search window is anchored to the scheduled TX timestamp (not to
     * whatever rx_meta.timestamp the first RX chunk happens to report),
     * so a burst landing earlier than expected is still inside the window:
     * one run at lead_ms=50 previously reported "no burst detected" because
     * the loop only checked an upper search_limit with no lower bound tied
     * to tx_meta.timestamp. */
    search_start_ts = tx_meta.timestamp - (bladerf_timestamp)RX_SEARCH_MARGIN_PRE_MS * (SAMPLERATE_HZ / 1000u);
    search_limit    = tx_meta.timestamp + (bladerf_timestamp)RX_SEARCH_MARGIN_MS * (SAMPLERATE_HZ / 1000u);
    printf("search_start_ts=%llu\n", (unsigned long long)search_start_ts);
    printf("search_end_ts=%llu\n", (unsigned long long)search_limit);

    memset(&rx_meta, 0, sizeof(rx_meta));
    rx_meta.flags = BLADERF_META_FLAG_RX_NOW;

    {
        double max_power_seen = 0.0;
        double median_seen     = 0.0;
        double threshold        = 0.0;
        bool first_chunk        = true;
        bool have_first_ts      = false;
        bladerf_timestamp rx_first_chunk_ts = 0;
        bladerf_timestamp rx_last_chunk_ts  = 0;
        bladerf_timestamp burst_start_ts    = 0;
        unsigned long long blocks_above     = 0;
        unsigned long long burst_len_blocks = 0;
        bool in_burst_run                   = false;
        int16_t *prev_rx_buf = malloc((size_t)BUFFER_SIZE * samples_per_frame * 2u * sizeof(int16_t));
        bool have_prev_chunk = false;
        bladerf_timestamp prev_chunk_ts = 0;

        if (prev_rx_buf == NULL) {
            fprintf(stderr, "alloc failed (prev_rx_buf)\n");
            ret = 1;
            goto out_disable_tx;
        }

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

            if (!have_first_ts) {
                rx_first_chunk_ts = rx_meta.timestamp;
                have_first_ts = true;
            }
            rx_last_chunk_ts = rx_meta.timestamp + n_blocks_per_chunk * BLOCK_SAMPLES;

            if (first_chunk) {
                double powers[BUFFER_SIZE / BLOCK_SAMPLES];
                unsigned int b;

                for (b = 0; b < n_blocks_per_chunk; b++) {
                    powers[b] = block_power(rx_buf, b, samples_per_frame, mimo ? 1u : 0u);
                }
                qsort(powers, n_blocks_per_chunk, sizeof(double), cmp_double);
                first_chunk_median = powers[n_blocks_per_chunk / 2];
                median_seen = first_chunk_median;
                threshold = POWER_RATIO_THRESH * median_seen;
                if (threshold < ABS_FLOOR) {
                    threshold = ABS_FLOOR;
                }
                first_chunk = false;
                printf("first_chunk_median_power=%.2f\n", first_chunk_median);
                printf("threshold=%.2f\n", threshold);
            }

            {
                unsigned int b;
                unsigned int pair_offset = mimo ? 1u : 0u;
                for (b = 0; b < n_blocks_per_chunk; b++) {
                    double p = block_power(rx_buf, b, samples_per_frame, pair_offset);
                    if (p > max_power_seen) {
                        max_power_seen = p;
                    }
                    if (threshold > 0.0 && p > threshold) {
                        bladerf_timestamp block_ts = rx_meta.timestamp + b * BLOCK_SAMPLES;

                        blocks_above++;
                        if (!in_burst_run) {
                            burst_start_ts = block_ts;
                            in_burst_run = true;
                            burst_len_blocks = 0;
                        }
                        burst_len_blocks++;

                        if (!burst_found) {
                            long long diff_samples = (long long)burst_start_ts - (long long)tx_meta.timestamp;
                            double diff_us = (double)diff_samples * 1e6 / (double)SAMPLERATE_HZ;
                            double edge_threshold = threshold / 2.0;
                            bladerf_timestamp burst_edge_ts = block_ts;
                            bool found_edge = false;

                            /* Sample-level edge scan: block detection only
                             * localizes to BLOCK_SAMPLES (25.6us at 10MS/s);
                             * scan the detected block, walking back into the
                             * previous block (possibly the previous RX
                             * chunk, via prev_rx_buf) for the first sample
                             * whose |I|+|Q| crosses half the block threshold. */
                            {
                                long long scan_frame;
                                long long block_start_frame = (long long)b * BLOCK_SAMPLES;
                                long long scan_from = block_start_frame - (long long)BLOCK_SAMPLES;

                                for (scan_frame = scan_from; scan_frame < block_start_frame + (long long)BLOCK_SAMPLES; scan_frame++) {
                                    const int16_t *src;
                                    long long local_frame;
                                    bladerf_timestamp sample_ts;
                                    int32_t iv, qv;
                                    double mag;

                                    if (scan_frame < 0) {
                                        if (!have_prev_chunk) {
                                            continue;
                                        }
                                        src = prev_rx_buf;
                                        local_frame = scan_frame + (long long)BUFFER_SIZE;
                                        sample_ts = prev_chunk_ts + (bladerf_timestamp)local_frame;
                                    } else if (scan_frame >= (long long)BUFFER_SIZE) {
                                        break;
                                    } else {
                                        src = rx_buf;
                                        local_frame = scan_frame;
                                        sample_ts = rx_meta.timestamp + (bladerf_timestamp)local_frame;
                                    }

                                    iv = src[2 * (local_frame * samples_per_frame + pair_offset)];
                                    qv = src[2 * (local_frame * samples_per_frame + pair_offset) + 1];
                                    mag = (double)labs(iv) + (double)labs(qv);

                                    if (mag > edge_threshold) {
                                        burst_edge_ts = sample_ts;
                                        found_edge = true;
                                        break;
                                    }
                                }
                            }

                            printf("rx_ts_burst_start=%llu\n", (unsigned long long)burst_start_ts);
                            printf("tx_scheduled_timestamp=%llu\n", (unsigned long long)tx_meta.timestamp);
                            printf("delta_samples=%lld\n", diff_samples);
                            printf("delta_us=%.2f\n", diff_us);
                            if (found_edge) {
                                long long edge_diff_samples = (long long)burst_edge_ts - (long long)tx_meta.timestamp;
                                double edge_diff_us = (double)edge_diff_samples * 1e6 / (double)SAMPLERATE_HZ;

                                printf("rx_ts_burst_edge=%llu\n", (unsigned long long)burst_edge_ts);
                                printf("delta_edge_samples=%lld\n", edge_diff_samples);
                                printf("delta_edge_us=%.2f\n", edge_diff_us);
                            } else {
                                printf("rx_ts_burst_edge=not_found\n");
                            }
                            burst_found = true;
                        }
                    } else {
                        in_burst_run = false;
                    }
                }
            }

            if (!burst_found && rx_meta.timestamp > search_limit) {
                printf("no burst detected\n");
                printf("max_block_power=%.2f\n", max_power_seen);
                printf("median_power=%.2f\n", median_seen);
                printf("threshold=%.2f\n", threshold);
                printf("rx_first_chunk_ts=%llu\n", (unsigned long long)rx_first_chunk_ts);
                printf("rx_last_chunk_ts=%llu\n", (unsigned long long)rx_last_chunk_ts);
                printf("blocks_above=%llu\n", blocks_above);
                break;
            }

            if (burst_found) {
                printf("burst_len_blocks=%llu\n", burst_len_blocks);
                printf("rx_first_chunk_ts=%llu\n", (unsigned long long)rx_first_chunk_ts);
                printf("rx_last_chunk_ts=%llu\n", (unsigned long long)rx_last_chunk_ts);
                printf("blocks_above=%llu\n", blocks_above);
            }

            if (!burst_found) {
                memcpy(prev_rx_buf, rx_buf, (size_t)BUFFER_SIZE * samples_per_frame * 2u * sizeof(int16_t));
                prev_chunk_ts = rx_meta.timestamp;
                have_prev_chunk = true;
            }
        }

        free(prev_rx_buf);
    }

    printf("overrun_seen=%d\n", (int)overrun_seen);
    ret = burst_found ? 0 : 1;

out_disable_tx:
    bladerf_enable_module(dev, BLADERF_CHANNEL_TX(ch), false);
    if (mimo) {
        bladerf_enable_module(dev, BLADERF_CHANNEL_TX(0), false);
    }
out_disable_rx:
    bladerf_enable_module(dev, BLADERF_CHANNEL_RX(ch), false);
    if (mimo) {
        bladerf_enable_module(dev, BLADERF_CHANNEL_RX(0), false);
    }
out_close:
    bladerf_close(dev);
out_free:
    free(tone_buf);
    free(tx_buf);
    free(rx_buf);
    return ret;
}
