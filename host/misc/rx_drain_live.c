/* Sustained RX host-data drain: continuous sync RX for a fixed duration after
 * one event-driven transition, with per-read latency and timestamp continuity.
 * Covers "drain host-data at 1.92 Msps" (bbfa6086 RX completion-gap trace is
 * emitted by libusb as "RX completion gap N ms" on stderr, grep the log) and,
 * with RX_X2 / 23040000 / 1 s, the 1-second LTE RX_X2 capture.
 *
 * Usage: [RX1|RX2|RX_X2] [rate_hz=1920000] [seconds=10] [block_samples=8192]
 *        [freq_hz=1835000000]
 * PASS (criteria fixed before the run):
 *   - every block: status 0, full count, no OVERRUN flag, epoch valid and
 *     equal to the transition epoch, timestamp == previous + samples/lane
 *   - samples read == ceil(seconds*rate/block)*block per lane
 *   - max gap between consecutive read completions <= 20 ms (the threshold
 *     bbfa6086 uses to log a USB completion gap); gaps are measured from
 *     the 2nd block on, so the first-read warm-up is excluded
 * Printed, not gated: p50/p99/max read gap, wall time, timestamp-rate. */
#include <libbladeRF.h>
#include "hwqual_live.h"

#include <sched.h>

#define GAP_LIMIT_MS 20.0

int main(int argc, char **argv)
{
    const char *layout_name = argc > 1 ? argv[1] : "RX1";
    const unsigned long rate = argc > 2 ? strtoul(argv[2], NULL, 10) : 1920000;
    const double seconds = argc > 3 ? strtod(argv[3], NULL) : 10.0;
    const unsigned block = argc > 4 ? (unsigned)strtoul(argv[4], NULL, 10)
                                    : 8192;
    const uint64_t freq = argc > 5 ? strtoull(argv[5], NULL, 10)
                                   : UINT64_C(1835000000);
    bladerf_channel_layout layout = BLADERF_RX_X1;
    bladerf_channel ch = BLADERF_CHANNEL_RX(0);
    unsigned lanes = 1;
    struct bladerf *dev = NULL;
    struct bladerf_rf_event ev = {0};
    struct bladerf_metadata meta;
    int16_t *buf = NULL;
    double *gaps = NULL;
    uint64_t blocks, expect_ts = 0, first_ts = 0, t_prev = 0, t_start = 0;
    uint64_t t_end = 0;
    size_t ngaps = 0;
    int s = 0;
    bool bad = false;

    if (strcmp(layout_name, "RX2") == 0) ch = BLADERF_CHANNEL_RX(1);
    else if (strcmp(layout_name, "RX_X2") == 0) {
        layout = BLADERF_RX_X2;
        lanes = 2;
    } else if (strcmp(layout_name, "RX1") != 0) {
        fprintf(stderr, "usage: %s [RX1|RX2|RX_X2] [rate] [seconds] [block] "
                "[freq]\n", argv[0]);
        return 2;
    }
    if (rate == 0 || rate > 61440000 || seconds <= 0 || block < 1024 ||
        block > 1048576 || block % 1024 != 0) {
        fputs("invalid rate/seconds/block\n", stderr);
        return 2;
    }
    blocks = (uint64_t)((seconds * (double)rate) / block + 0.999999);
    buf = calloc((size_t)block * lanes * 2, sizeof(*buf));
    gaps = calloc(blocks, sizeof(*gaps));
    if (buf == NULL || gaps == NULL) return 2;

    s = bladerf_open(&dev, NULL);
    if (s != 0) {
        hq_fail("open", s);
        return 1;
    }
    if ((s = bladerf_set_sample_rate(dev, ch, (bladerf_sample_rate)rate,
                                     NULL)) != 0 ||
        (s = bladerf_set_bandwidth(dev, ch, rate > 23040000 ? 20000000u
                                   : (unsigned)(rate * 3 / 4), NULL)) != 0 ||
        (s = bladerf_set_frequency(dev, ch, freq)) != 0 ||
        (s = bladerf_sync_config(dev, layout, BLADERF_FORMAT_SC16_Q11_META,
                                 32, block, 16, 3000)) != 0 ||
        (s = bladerf_enable_module(dev, BLADERF_CHANNEL_RX(0), true)) != 0 ||
        (lanes == 2 &&
         (s = bladerf_enable_module(dev, BLADERF_CHANNEL_RX(1), true)) != 0)) {
        hq_fail("configure", s);
        goto out;
    }
    if ((s = hq_transition(dev, ch, freq, 2000, &ev)) != 0) {
        hq_fail("transition", s);
        goto out;
    }
    printf("START layout=%s rate=%lu seconds=%.3f block=%u blocks=%" PRIu64
           " epoch=%u\n", layout_name, rate, seconds, block, blocks,
           ev.epoch_id);

    for (uint64_t i = 0; i < blocks && !bad; ++i) {
        uint64_t now;
        meta = (struct bladerf_metadata){ .flags = BLADERF_META_FLAG_RX_NOW };
        s = bladerf_sync_rx(dev, buf, block * lanes, &meta, 3000);
        now = hq_mono_ns();
        if (i == 0) t_start = now;
        if (s != 0 || meta.actual_count != block * lanes ||
            (meta.status & BLADERF_META_STATUS_OVERRUN) ||
            !meta.rx_epoch_id_valid || meta.rx_epoch_id != ev.epoch_id) {
            fprintf(stderr, "BLOCK_INVALID i=%" PRIu64 " status=%s count=%u/%u "
                    "meta_status=0x%x epoch=%u/%u valid=%u\n", i,
                    bladerf_strerror(s), meta.actual_count, block * lanes,
                    meta.status, meta.rx_epoch_id, ev.epoch_id,
                    meta.rx_epoch_id_valid);
            bad = true;
            break;
        }
        if (i == 0) {
            first_ts = meta.timestamp;
        } else {
            gaps[ngaps++] = (now - t_prev) / 1e6;
            if (meta.timestamp != expect_ts) {
                fprintf(stderr, "TIMESTAMP_GAP i=%" PRIu64 " delta=%" PRId64
                        "\n", i, (int64_t)(meta.timestamp - expect_ts));
                bad = true;
            }
        }
        expect_ts = meta.timestamp + block;
        t_prev = now;
        t_end = now;
    }
    if (!bad && ngaps > 0) {
        const double wall_s = (t_end - t_start) / 1e9;
        const double ts_rate = wall_s > 0 ?
            (double)(expect_ts - first_ts - block) / wall_s : 0.0;
        hq_print_stats("read_gap_ms", gaps, ngaps);
        printf("WALL blocks=%" PRIu64 " wall_s=%.3f timestamp_rate_hz=%.1f "
               "requested_hz=%lu\n", blocks, wall_s, ts_rate, rate);
        if (gaps[ngaps - 1] > GAP_LIMIT_MS) {
            fprintf(stderr, "READ_GAP max %.3f ms > %.0f ms\n",
                    gaps[ngaps - 1], GAP_LIMIT_MS);
            bad = true;
        }
    }
    if (!bad && s == 0) printf("PASS layout=%s rate=%lu blocks=%" PRIu64 "\n",
                               layout_name, rate, blocks);
    else if (s == 0) s = BLADERF_ERR_UNEXPECTED;
    if (s != 0) printf("FAIL layout=%s rate=%lu\n", layout_name, rate);
out:
    if (dev != NULL) {
        (void)bladerf_enable_module(dev, BLADERF_CHANNEL_RX(1), false);
        (void)bladerf_enable_module(dev, BLADERF_CHANNEL_RX(0), false);
        bladerf_close(dev);
    }
    free(buf);
    free(gaps);
    return (s == 0 && !bad) ? 0 : 1;
}
