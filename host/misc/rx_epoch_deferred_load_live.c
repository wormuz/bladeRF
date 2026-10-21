/* Deferred (scheduled) RX retune activation while a reader thread streams.
 *
 * Per trial: queue DEPTH scheduled quick-tune retunes at now + k*SPACING
 * samples (alternating two profiles), wait until the device timestamp is past
 * the last one, then establish a new epoch with an event-driven transition.
 * A reader thread runs sync RX the whole time (the load) and checks every
 * block it receives. Scheduling revokes the RX epoch (libbladeRF.h), so
 * reads between the schedule and the new epoch are allowed to be withheld.
 *
 * Usage: [trials, default 50] [rate_hz, default 7680000]
 * PASS (criteria fixed before the run):
 *   - every schedule_retune returns 0 and every transition completes
 *   - zero BLADERF_RF_EVT_RX_RETUNE_FAILED events and a complete event
 *     history over the run
 *   - reader: no OVERRUN block, no block with a hard error other than
 *     TIMEOUT/WOULD_BLOCK (withheld IQ), epoch ids never go backwards
 *   - after each transition the reader receives a block of the new epoch
 *     within 2 s
 * Printed: blocks, withheld reads, p50/p99/max schedule-to-new-epoch ms. */
#include <libbladeRF.h>
#include "hwqual_live.h"

#include <pthread.h>
#include <stdatomic.h>

#define RX_CH BLADERF_CHANNEL_RX(0)
#define FREQ_A UINT64_C(1835000000)
#define FREQ_B UINT64_C(1835400000)
#define DEPTH 4u
#define SPACING_MS 20u
#define BLOCK 8192u

struct reader {
    struct bladerf *dev;
    atomic_bool run;
    atomic_uint last_epoch;
    atomic_uint blocks, withheld, overruns, hard_errors, regress;
    atomic_int first_hard;
};

static void *reader_main(void *arg)
{
    struct reader *r = arg;
    int16_t *buf = calloc(BLOCK * 2, sizeof(*buf));
    uint32_t prev = 0;
    bool have_prev = false;

    while (atomic_load(&r->run)) {
        struct bladerf_metadata m = { .flags = BLADERF_META_FLAG_RX_NOW };
        const int s = bladerf_sync_rx(r->dev, buf, BLOCK, &m, 100);
        if (s == BLADERF_ERR_TIMEOUT || s == BLADERF_ERR_WOULD_BLOCK) {
            atomic_fetch_add(&r->withheld, 1);
            continue;
        }
        if (s != 0) {
            atomic_fetch_add(&r->hard_errors, 1);
            atomic_store(&r->first_hard, s);
            break;
        }
        if (m.status & BLADERF_META_STATUS_OVERRUN)
            atomic_fetch_add(&r->overruns, 1);
        if (m.rx_epoch_id_valid) {
            if (have_prev && (int32_t)(m.rx_epoch_id - prev) < 0)
                atomic_fetch_add(&r->regress, 1);
            prev = m.rx_epoch_id;
            have_prev = true;
            atomic_store(&r->last_epoch, m.rx_epoch_id);
        }
        atomic_fetch_add(&r->blocks, 1);
    }
    free(buf);
    return NULL;
}

int main(int argc, char **argv)
{
    const unsigned trials = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 10)
                                     : 50;
    const unsigned long rate = argc > 2 ? strtoul(argv[2], NULL, 10) : 7680000;
    const uint64_t spacing = (uint64_t)rate * SPACING_MS / 1000u;
    struct bladerf *dev = NULL;
    struct bladerf_quick_tune qt[2];
    struct reader rd;
    pthread_t th;
    double *lat = calloc(trials ? trials : 1, sizeof(*lat));
    uint64_t cursor = 0;
    unsigned failed_events = 0, done = 0;
    bool history_complete = true, ok = true, reader_started = false;
    int s;

    if (trials == 0 || trials > 10000 || rate < 1000000 || rate > 61440000 ||
        lat == NULL)
        return 2;
    atomic_init(&rd.run, true);
    atomic_init(&rd.last_epoch, 0);
    atomic_init(&rd.blocks, 0);
    atomic_init(&rd.withheld, 0);
    atomic_init(&rd.overruns, 0);
    atomic_init(&rd.hard_errors, 0);
    atomic_init(&rd.regress, 0);
    atomic_init(&rd.first_hard, 0);

    if ((s = bladerf_open(&dev, NULL)) != 0) {
        hq_fail("open", s);
        return 1;
    }
    rd.dev = dev;
    if (hq_require_trigger_disarmed(dev) != 0) {
        bladerf_close(dev);
        return 3;
    }
    if ((s = bladerf_set_sample_rate(dev, RX_CH, (bladerf_sample_rate)rate,
                                     NULL)) != 0 ||
        (s = bladerf_set_bandwidth(dev, RX_CH, (unsigned)(rate * 3 / 4),
                                   NULL)) != 0 ||
        (s = bladerf_set_frequency(dev, RX_CH, FREQ_A)) != 0 ||
        (s = bladerf_get_quick_tune(dev, RX_CH, &qt[0])) != 0 ||
        (s = bladerf_set_frequency(dev, RX_CH, FREQ_B)) != 0 ||
        (s = bladerf_get_quick_tune(dev, RX_CH, &qt[1])) != 0 ||
        (s = bladerf_set_frequency(dev, RX_CH, FREQ_A)) != 0 ||
        (s = bladerf_sync_config(dev, BLADERF_RX_X1,
                                 BLADERF_FORMAT_SC16_Q11_META, 32, BLOCK, 16,
                                 3000)) != 0 ||
        (s = bladerf_enable_module(dev, RX_CH, true)) != 0) {
        hq_fail("configure", s);
        ok = false;
        goto out;
    }
    {
        struct bladerf_rf_event first = {0};
        unsigned ignore = 0;
        if ((s = hq_transition(dev, RX_CH, FREQ_A, 2000, &first)) != 0) {
            hq_fail("initial transition", s);
            ok = false;
            goto out;
        }
        (void)hq_count_events(dev, &cursor, BLADERF_RF_EVT_RX_RETUNE_FAILED,
                              &ignore, &history_complete);
        history_complete = true;
    }
    pthread_create(&th, NULL, reader_main, &rd);
    reader_started = true;

    for (unsigned i = 0; ok && i < trials; ++i) {
        struct bladerf_rf_event ev = {0};
        bladerf_timestamp now = 0, last = 0;
        uint64_t t0 = hq_mono_ns(), t_wait;

        s = bladerf_get_timestamp(dev, BLADERF_RX, &now);
        for (unsigned k = 0; s == 0 && k < DEPTH; ++k) {
            last = now + (k + 1) * spacing;
            s = bladerf_schedule_retune(dev, RX_CH, last,
                                        (k & 1u) ? FREQ_B : FREQ_A,
                                        &qt[(k & 1u) ? 1 : 0]);
        }
        t_wait = hq_mono_ns();
        while (s == 0) {
            bladerf_timestamp cur = 0;
            s = bladerf_get_timestamp(dev, BLADERF_RX, &cur);
            if (s == 0 && (int64_t)(cur - last) > (int64_t)(spacing / 2))
                break;
            if (hq_mono_ns() - t_wait > UINT64_C(5000000000)) {
                s = BLADERF_ERR_TIMEOUT;
                break;
            }
        }
        if (s == 0) s = hq_transition(dev, RX_CH, FREQ_A, 2000, &ev);
        if (s == 0) {
            const uint64_t t1 = hq_mono_ns();
            while (atomic_load(&rd.last_epoch) != ev.epoch_id &&
                   hq_mono_ns() - t1 < UINT64_C(2000000000)) {
                struct timespec ts = { 0, 200000 };
                nanosleep(&ts, NULL);
            }
            if (atomic_load(&rd.last_epoch) != ev.epoch_id) {
                fprintf(stderr, "trial %u: reader never saw epoch %u (last "
                        "%u)\n", i, ev.epoch_id, atomic_load(&rd.last_epoch));
                ok = false;
                break;
            }
            lat[done++] = (hq_mono_ns() - t0) / 1e6;
        } else {
            hq_fail("trial", s);
            ok = false;
            break;
        }
        s = hq_count_events(dev, &cursor, BLADERF_RF_EVT_RX_RETUNE_FAILED,
                            &failed_events, &history_complete);
        if (s != 0 || !history_complete) {
            fprintf(stderr, "trial %u: event history s=%s complete=%u\n", i,
                    bladerf_strerror(s), history_complete);
            ok = false;
        }
    }

out:
    if (reader_started) {
        atomic_store(&rd.run, false);
        pthread_join(th, NULL);
    }
    if (dev != NULL) {
        (void)bladerf_cancel_scheduled_retunes(dev, RX_CH);
        (void)bladerf_enable_module(dev, RX_CH, false);
        bladerf_close(dev);
    }
    if (done > 0) hq_print_stats("schedule_to_new_epoch_ms", lat, done);
    printf("TOTAL trials=%u completed=%u blocks=%u withheld=%u overruns=%u "
           "hard_errors=%u(first=%d) epoch_regress=%u retune_failed=%u\n",
           trials, done, atomic_load(&rd.blocks), atomic_load(&rd.withheld),
           atomic_load(&rd.overruns), atomic_load(&rd.hard_errors),
           atomic_load(&rd.first_hard), atomic_load(&rd.regress),
           failed_events);
    ok = ok && done == trials && failed_events == 0 &&
         atomic_load(&rd.overruns) == 0 && atomic_load(&rd.hard_errors) == 0 &&
         atomic_load(&rd.regress) == 0;
    printf("%s\n", ok ? "PASS" : "FAIL");
    free(lat);
    return ok ? 0 : 1;
}
