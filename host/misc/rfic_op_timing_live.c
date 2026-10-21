/* NIOS service-gap / worker-callback / command-lifetime measurement for the
 * RFIC operations rfic_init_timing_live.c does not cover (cold INIT is its
 * job): RX RETUNE2 (fastlock transition), TX RETUNE2, TXQ (TX LO set with
 * quad calibration), sample rate, and fastlock profile loader step.
 *
 * Mechanism: the host opens the NIOS operation window (nios_op_window.h,
 * RFIC_TIMING target OPWIN registers) around each phase; NIOS records max
 * deferred-worker callback, max gap between main-loop deferred passes, max
 * service-hook gap and duration in device-clock ticks. The host times each
 * command from submit to completion and compares with the host response
 * watchdogs from nios_access.c: RX 5 s, TX 120 s.
 *
 * Needs an image with the operation window; on an older image the first
 * window write fails and the tool exits 3 (NOT_EVALUATED), not PASS.
 * TX phases enable the TX module with no samples queued and TX gain at its
 * minimum.
 *
 * PASS: every operation returned success; window valid (hz>0, passes>0,
 * max_worker and max_service_gap <= window wall time (host clock, in ticks)); per phase the host
 * lifetime max and NIOS max service gap are below the phase watchdog
 * (rx/samplerate/fastlock_load 5000 ms, tx/txq 120000 ms). Maxima are a
 * sample, not a WCET bound.
 * Usage: [serial] [reps:1..1000] [phases: rx,tx,txq,samplerate,fastlock_load] */
#include <libbladeRF.h>
#include "rfic_timing_access.h"
#include "hwqual_live.h"

#include <time.h>

#define RX_CH BLADERF_CHANNEL_RX(0)
#define TX_CH BLADERF_CHANNEL_TX(0)
#define RATE_A 3840000u
#define RATE_B 1920000u
#define BANDWIDTH 1500000u
#define FREQ_A UINT64_C(1835000000)
#define FREQ_B UINT64_C(1835400000)
#define RX_WATCHDOG_MS 5000.0
#define TX_WATCHDOG_MS 120000.0
#define PHASE_EVENT_TIMEOUT_NS UINT64_C(130000000000)

struct phase_result {
    double *lifetime_us;
    size_t n;
};

static void pause_us(long us)
{
    struct timespec ts = { 0, us * 1000L };
    nanosleep(&ts, NULL);
}

static int rx_start(struct bladerf *dev)
{
    int s;
    if ((s = bladerf_set_sample_rate(dev, RX_CH, RATE_A, NULL)) != 0 ||
        (s = bladerf_set_bandwidth(dev, RX_CH, BANDWIDTH, NULL)) != 0 ||
        (s = bladerf_sync_config(dev, BLADERF_RX_X1,
                                 BLADERF_FORMAT_SC16_Q11_META, 16, 8192, 8,
                                 0)) != 0 ||
        (s = bladerf_enable_module(dev, RX_CH, true)) != 0)
        return hq_fail("rx start", s);
    return 0;
}

static int get_qt(struct bladerf *dev, bladerf_channel ch,
                  struct bladerf_quick_tune qt[2])
{
    int s;
    if ((s = bladerf_set_frequency(dev, ch, FREQ_A)) != 0 ||
        (s = bladerf_get_quick_tune(dev, ch, &qt[0])) != 0 ||
        (s = bladerf_set_frequency(dev, ch, FREQ_B)) != 0 ||
        (s = bladerf_get_quick_tune(dev, ch, &qt[1])) != 0 ||
        (s = bladerf_set_frequency(dev, ch, FREQ_A)) != 0)
        return hq_fail("quick tune", s);
    return 0;
}

static int op_rx(struct bladerf *dev, unsigned reps, double *life)
{
    struct bladerf_quick_tune qt[2];
    int s = rx_start(dev);
    if (s == 0) s = get_qt(dev, RX_CH, qt);
    for (unsigned i = 0; s == 0 && i < reps; ++i) {
        const unsigned t = (i + 1) & 1u;
        const struct bladerf_rx_transition_request rq =
            hq_rx_request(t ? FREQ_B : FREQ_A, 0);
        struct bladerf_rf_event ev = {0};
        uint32_t txn = 0;
        const uint64_t t0 = hq_mono_ns();
        s = bladerf_rx_transition_begin_quick_tune(dev, RX_CH, &rq, &qt[t],
                                                   &txn);
        if (s == 0) s = bladerf_rx_transition_wait(dev, txn, &ev, 0);
        life[i] = (hq_mono_ns() - t0) / 1000.0;
    }
    (void)bladerf_enable_module(dev, RX_CH, false);
    return s;
}

static int op_samplerate(struct bladerf *dev, unsigned reps, double *life)
{
    int s = 0;
    for (unsigned i = 0; s == 0 && i < reps; ++i) {
        const bladerf_sample_rate want = (i & 1u) ? RATE_B : RATE_A;
        bladerf_sample_rate got = 0;
        const uint64_t t0 = hq_mono_ns();
        s = bladerf_set_sample_rate(dev, RX_CH, want, &got);
        life[i] = (hq_mono_ns() - t0) / 1000.0;
        if (s == 0 && got != want) {
            fprintf(stderr, "sample rate %u requested, %u actual\n", want,
                    got);
            s = BLADERF_ERR_UNEXPECTED;
        }
    }
    return s;
}

static int op_fastlock_load(struct bladerf *dev, unsigned reps, double *life)
{
    struct bladerf_quick_tune qt[2];
    uint8_t profile[2][BLADERF_QUICK_TUNE_PROFILE_LEN];
    int s = rx_start(dev);
    if (s == 0) s = get_qt(dev, RX_CH, qt);
    for (int k = 0; s == 0 && k < 2; ++k) {
        s = bladerf_get_quick_tune_profile(dev, RX_CH, &qt[k], profile[k]);
        if (s != 0) hq_fail("get_quick_tune_profile", s);
    }
    for (unsigned i = 0; s == 0 && i < reps; ++i) {
        const unsigned t = i & 1u;
        const struct bladerf_rx_transition_request rq =
            hq_rx_request(t ? FREQ_B : FREQ_A, 0);
        struct bladerf_quick_tune loaded;
        struct bladerf_rf_event ev = {0};
        uint32_t txn = 0;
        const uint64_t t0 = hq_mono_ns();
        /* A fresh NIOS slot per repetition makes every recall upload the
         * profile words into the RFIC (the loader step). */
        s = bladerf_load_quick_tune_profile(
            dev, RX_CH, profile[t],
            (uint16_t)(i % BLADERF_QUICK_TUNE_NIOS_SLOTS), &loaded);
        if (s == 0)
            s = bladerf_rx_transition_begin_quick_tune(dev, RX_CH, &rq,
                                                       &loaded, &txn);
        if (s == 0) s = bladerf_rx_transition_wait(dev, txn, &ev, 0);
        life[i] = (hq_mono_ns() - t0) / 1000.0;
    }
    (void)bladerf_enable_module(dev, RX_CH, false);
    return s;
}

static int op_txq(struct bladerf *dev, unsigned reps, double *life)
{
    int s = 0;
    for (unsigned i = 0; s == 0 && i < reps; ++i) {
        const uint64_t t0 = hq_mono_ns();
        s = bladerf_set_frequency(dev, TX_CH, (i & 1u) ? FREQ_B : FREQ_A);
        life[i] = (hq_mono_ns() - t0) / 1000.0;
    }
    return s;
}

static int op_tx(struct bladerf *dev, unsigned reps, double *life)
{
    struct bladerf_quick_tune qt[2];
    const struct bladerf_range *range = NULL;
    int s;
    uint64_t cursor = 0;
    unsigned ignore = 0;
    bool complete = true;

    s = bladerf_get_gain_range(dev, TX_CH, &range);
    if (s == 0)
        s = bladerf_set_gain(dev, TX_CH, (int)range->min);
    if (s == 0) s = get_qt(dev, TX_CH, qt);
    if (s == 0)
        s = bladerf_sync_config(dev, BLADERF_TX_X1,
                                BLADERF_FORMAT_SC16_Q11_META, 16, 8192, 8, 0);
    if (s == 0) s = bladerf_enable_module(dev, TX_CH, true);
    if (s != 0) return hq_fail("tx start", s);
    s = hq_count_events(dev, &cursor, BLADERF_RF_EVT_TX_RETUNE_VALID, &ignore,
                        &complete);
    for (unsigned i = 0; s == 0 && i < reps; ++i) {
        const unsigned t = (i + 1) & 1u;
        unsigned valid = 0, failed = 0;
        const uint64_t t0 = hq_mono_ns();
        s = bladerf_schedule_retune(dev, TX_CH, BLADERF_RETUNE_NOW,
                                    t ? FREQ_B : FREQ_A, &qt[t]);
        while (s == 0 && valid == 0) {
            if (hq_mono_ns() - t0 > PHASE_EVENT_TIMEOUT_NS) {
                fprintf(stderr, "TX retune %u: no terminal event\n", i);
                s = BLADERF_ERR_TIMEOUT;
                break;
            }
            s = hq_count_events(dev, &cursor, BLADERF_RF_EVT_TX_RETUNE_VALID,
                                &valid, &complete);
            if (s == 0)
                s = hq_count_events(dev, &cursor,
                                    BLADERF_RF_EVT_TX_RETUNE_FAILED, &failed,
                                    &complete);
            if (s == 0 && failed) {
                fprintf(stderr, "TX retune %u: failed event\n", i);
                s = BLADERF_ERR_UNEXPECTED;
            }
            if (s == 0 && valid == 0) pause_us(200);
        }
        life[i] = (hq_mono_ns() - t0) / 1000.0;
    }
    (void)bladerf_enable_module(dev, TX_CH, false);
    return s;
}

static int run_phase(struct bladerf *dev, const char *name, unsigned reps,
                     double watchdog_ms,
                     int (*op)(struct bladerf *, unsigned, double *),
                     bool *window_unsupported)
{
    double *life = calloc(reps, sizeof(*life));
    uint64_t hz = 0, worker = 0, gap = 0, hook_gap = 0, hook = 0, elapsed = 0,
             passes = 0, t_open, t_close;
    int s, rs = 0;
    bool ok;

    if (life == NULL) return BLADERF_ERR_MEM;
    s = write_metric(dev, NIOS_PKT_8x64_OPWIN_CONTROL, 1);
    if (s != 0) {
        *window_unsupported = true;
        fprintf(stderr, "operation window not supported by this image: %s\n",
                bladerf_strerror(s));
        free(life);
        return s;
    }
    t_open = hq_mono_ns();
    s = op(dev, reps, life);
    rs = write_metric(dev, NIOS_PKT_8x64_OPWIN_CONTROL, 0);
    t_close = hq_mono_ns();
    if (rs == 0)
        rs = read_metric(dev, NIOS_PKT_8x64_OPWIN_MAX_WORKER_TICKS, &worker);
    if (rs == 0)
        rs = read_metric(dev, NIOS_PKT_8x64_OPWIN_MAX_SERVICE_GAP_TICKS, &gap);
    if (rs == 0)
        rs = read_metric(dev, NIOS_PKT_8x64_OPWIN_MAX_HOOK_GAP_TICKS,
                         &hook_gap);
    if (rs == 0)
        rs = read_metric(dev, NIOS_PKT_8x64_OPWIN_MAX_HOOK_TICKS, &hook);
    if (rs == 0)
        rs = read_metric(dev, NIOS_PKT_8x64_OPWIN_PASSES, &passes);
    if (rs == 0)
        rs = read_metric(dev, NIOS_PKT_8x64_RFIC_TIMING_COUNTER_HZ, &hz);
    if (s != 0) hq_fail(name, s);
    if (rs != 0) hq_fail("window read", rs);

    elapsed = hz ? (uint64_t)((double)(t_close - t_open) * (double)hz / 1e9) : 0;
    ok = s == 0 && rs == 0 && hz != 0 && passes != 0 && worker <= elapsed &&
         gap <= elapsed;
    if (ok) {
        const double ms = 1000.0 / (double)hz;
        double lmax;
        hq_print_stats(name, life, reps);
        lmax = life[reps - 1] / 1000.0; /* sorted by hq_print_stats */
        printf("PHASE %s reps=%u lifetime_max_ms=%.3f watchdog_ms=%.0f "
               "nios_max_worker_ms=%.3f nios_max_service_gap_ms=%.3f "
               "nios_max_hook_gap_ms=%.3f nios_max_hook_ms=%.3f "
               "window_ms=%.3f passes=%" PRIu64 " hz=%" PRIu64 "\n",
               name, reps, lmax, watchdog_ms, (double)worker * ms,
               (double)gap * ms, (double)hook_gap * ms, (double)hook * ms,
               (double)elapsed * ms, passes, hz);
        if (lmax >= watchdog_ms || (double)gap * ms >= watchdog_ms) {
            fprintf(stderr, "PHASE %s: lifetime or service gap reaches the "
                    "watchdog\n", name);
            ok = false;
        }
    } else {
        fprintf(stderr, "PHASE %s invalid: s=%d rs=%d hz=%" PRIu64
                " passes=%" PRIu64 " worker=%" PRIu64 " gap=%" PRIu64
                " elapsed=%" PRIu64 "\n", name, s, rs, hz, passes, worker,
                gap, elapsed);
    }
    free(life);
    return ok ? 0 : (s != 0 ? s : BLADERF_ERR_UNEXPECTED);
}

int main(int argc, char **argv)
{
    static const struct {
        const char *name;
        double watchdog_ms;
        int (*op)(struct bladerf *, unsigned, double *);
    } phases[] = {
        {"rx", RX_WATCHDOG_MS, op_rx},
        {"tx", TX_WATCHDOG_MS, op_tx},
        {"txq", TX_WATCHDOG_MS, op_txq},
        {"samplerate", RX_WATCHDOG_MS, op_samplerate},
        {"fastlock_load", RX_WATCHDOG_MS, op_fastlock_load},
    };
    const char *serial = argc > 1 ? argv[1] : NULL;
    const unsigned reps = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 50;
    const char *want = argc > 3 ? argv[3] : "rx,tx,txq,samplerate,fastlock_load";
    char spec[128] = "";
    struct bladerf *dev = NULL;
    bool unsupported = false;
    int failed = 0, s;

    if (reps == 0 || reps > 1000 || argc > 4) {
        fprintf(stderr, "usage: %s [serial] [reps:1..1000] [phases]\n",
                argv[0]);
        return 2;
    }
    if (serial != NULL && serial[0] != '\0')
        snprintf(spec, sizeof(spec), "*:serial=%s", serial);
    s = bladerf_open(&dev, spec[0] ? spec : NULL);
    if (s != 0) {
        hq_fail("open", s);
        return 1;
    }

    for (size_t p = 0; p < sizeof(phases) / sizeof(phases[0]); ++p) {
        char key[32];
        snprintf(key, sizeof(key), ",%s,", phases[p].name);
        char list[128];
        snprintf(list, sizeof(list), ",%s,", want);
        if (strstr(list, key) == NULL) continue;
        s = run_phase(dev, phases[p].name, reps, phases[p].watchdog_ms,
                      phases[p].op, &unsupported);
        if (unsupported) break;
        if (s != 0) ++failed;
    }
    bladerf_close(dev);
    if (unsupported) {
        puts("NOT_EVALUATED operation window missing in loaded image");
        return 3;
    }
    printf("%s failed_phases=%d\n", failed ? "FAIL" : "PASS", failed);
    return failed ? 1 : 0;
}
