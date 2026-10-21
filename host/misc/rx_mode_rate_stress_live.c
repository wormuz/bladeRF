/* Concurrent feature-mode and sample-rate changes. Thread A toggles
 * BLADERF_FEATURE_OVERSAMPLE/DEFAULT, thread B alternates the RX sample rate,
 * both for a fixed iteration count with no pacing; a hang is caught by the
 * caller's timeout (run script: timeout on the step).
 *
 * Usage: [iterations_per_thread, default 100] [seed, 0 = time]
 * PASS (criteria fixed before the run):
 *   - every call returns 0 or a fail-closed rejection (WOULD_BLOCK, INVAL,
 *     UNSUPPORTED, RANGE: the rate range depends on the feature mode,
 *     bladerf2.c:2391); any other status (IO, TIMEOUT, FPGA_OP, UNEXPECTED, ...)
 *     is a FAIL
 *   - after the threads join and the feature is reset to DEFAULT: sample
 *     rate set to 3840000 reads back 3840000, get_feature == DEFAULT, a fresh
 *     event-driven RX transition succeeds, and one sync RX block is full,
 *     epoch-valid with the new epoch and without OVERRUN
 * Printed: accepted/rejected counts per thread. The PRNG seed (which thread
 * is created first) is printed. */
#include <libbladeRF.h>
#include "hwqual_live.h"

#include <pthread.h>

#define RX_CH BLADERF_CHANNEL_RX(0)
#define RATE_A 3840000u
#define RATE_B 1920000u
#define FREQ UINT64_C(1835000000)
#define BLOCK 8192u

struct worker {
    struct bladerf *dev;
    unsigned iterations;
    unsigned ok, rejected;
    int fatal;
    int fatal_iter;
    pthread_barrier_t *barrier;
    bool feature_thread;
};

static bool fail_closed(int s)
{
    return s == BLADERF_ERR_WOULD_BLOCK || s == BLADERF_ERR_INVAL ||
           s == BLADERF_ERR_RANGE ||
           s == BLADERF_ERR_UNSUPPORTED;
}

static void *worker_main(void *arg)
{
    struct worker *w = arg;
    pthread_barrier_wait(w->barrier);
    for (unsigned i = 0; i < w->iterations; ++i) {
        int s;
        if (w->feature_thread)
            s = bladerf_enable_feature(
                w->dev, (i & 1u) ? BLADERF_FEATURE_DEFAULT
                                 : BLADERF_FEATURE_OVERSAMPLE, true);
        else
            s = bladerf_set_sample_rate(w->dev, RX_CH,
                                        (i & 1u) ? RATE_B : RATE_A, NULL);
        if (s == 0) ++w->ok;
        else if (fail_closed(s)) ++w->rejected;
        else {
            w->fatal = s;
            w->fatal_iter = (int)i;
            break;
        }
    }
    return NULL;
}

int main(int argc, char **argv)
{
    const unsigned iterations = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 10)
                                         : 100;
    uint64_t seed = hq_seed_or_time(argc > 2 ? strtoull(argv[2], NULL, 0) : 0);
    uint64_t rng = seed;
    struct bladerf *dev = NULL;
    pthread_barrier_t barrier;
    pthread_t th[2];
    struct worker w[2];
    struct bladerf_rf_event ev = {0};
    struct bladerf_metadata meta = { .flags = BLADERF_META_FLAG_RX_NOW };
    bladerf_feature feature = BLADERF_FEATURE_OVERSAMPLE;
    bladerf_sample_rate got = 0;
    int16_t *buf = calloc(BLOCK * 2, sizeof(*buf));
    int s;
    bool ok = true;

    if (iterations == 0 || iterations > 100000 || buf == NULL) return 2;
    printf("seed=%" PRIu64 " iterations=%u\n", seed, iterations);
    if ((s = bladerf_open(&dev, NULL)) != 0) {
        hq_fail("open", s);
        return 1;
    }
    if ((s = hq_require_trigger_disarmed(dev)) != 0) {
        bladerf_close(dev);
        return 3;
    }
    if ((s = bladerf_set_sample_rate(dev, RX_CH, RATE_A, NULL)) != 0 ||
        (s = bladerf_set_frequency(dev, RX_CH, FREQ)) != 0) {
        hq_fail("prepare", s);
        bladerf_close(dev);
        return 1;
    }

    pthread_barrier_init(&barrier, NULL, 2);
    for (int i = 0; i < 2; ++i) {
        w[i] = (struct worker){ .dev = dev, .iterations = iterations,
                                .barrier = &barrier,
                                .feature_thread = (i == 0), .fatal_iter = -1 };
    }
    {
        const int first = (int)(hq_rng_next(&rng) >> 63);
        pthread_create(&th[first], NULL, worker_main, &w[first]);
        pthread_create(&th[!first], NULL, worker_main, &w[!first]);
    }
    pthread_join(th[0], NULL);
    pthread_join(th[1], NULL);
    pthread_barrier_destroy(&barrier);

    for (int i = 0; i < 2; ++i) {
        printf("THREAD %s ok=%u rejected=%u fatal=%d\n",
               i == 0 ? "feature" : "rate", w[i].ok, w[i].rejected,
               w[i].fatal);
        if (w[i].fatal != 0) {
            fprintf(stderr, "thread %d iteration %d: %s\n", i, w[i].fatal_iter,
                    bladerf_strerror(w[i].fatal));
            ok = false;
        }
    }

    if ((s = bladerf_enable_feature(dev, BLADERF_FEATURE_DEFAULT, true)) != 0 ||
        (s = bladerf_set_sample_rate(dev, RX_CH, RATE_A, &got)) != 0 ||
        (s = bladerf_get_feature(dev, &feature)) != 0) {
        hq_fail("recover", s);
        ok = false;
    } else if (got != RATE_A || feature != BLADERF_FEATURE_DEFAULT) {
        fprintf(stderr, "recovered state wrong: rate=%u feature=%d\n", got,
                (int)feature);
        ok = false;
    }
    if (ok) {
        s = bladerf_get_sample_rate(dev, RX_CH, &got);
        if (s == 0 && got != RATE_A) s = BLADERF_ERR_UNEXPECTED;
        if (s == 0) s = bladerf_set_bandwidth(dev, RX_CH, 1500000u, NULL);
        if (s == 0)
            s = bladerf_sync_config(dev, BLADERF_RX_X1,
                                    BLADERF_FORMAT_SC16_Q11_META, 16, BLOCK, 8,
                                    3000);
        if (s == 0) s = bladerf_enable_module(dev, RX_CH, true);
        if (s == 0) s = hq_transition(dev, RX_CH, FREQ, 2000, &ev);
        if (s == 0) s = bladerf_sync_rx(dev, buf, BLOCK, &meta, 3000);
        if (s == 0 && (meta.actual_count != BLOCK ||
                       (meta.status & BLADERF_META_STATUS_OVERRUN) ||
                       !meta.rx_epoch_id_valid ||
                       meta.rx_epoch_id != ev.epoch_id))
            s = BLADERF_ERR_UNEXPECTED;
        if (s != 0) {
            hq_fail("post-stress RX", s);
            ok = false;
        } else {
            printf("RECOVERED rate=%u epoch=%u\n", got, ev.epoch_id);
        }
    }
    (void)bladerf_enable_module(dev, RX_CH, false);
    bladerf_close(dev);
    free(buf);
    printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
