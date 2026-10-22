/* Shared helpers for the live qualification tools (header-only, no state). */
#ifndef HWQUAL_LIVE_H_
#define HWQUAL_LIVE_H_

#include <libbladeRF.h>

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static inline uint64_t hq_mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

/* xorshift64*: the tools print the seed so a run can be replayed. */
static inline uint64_t hq_rng_next(uint64_t *s)
{
    *s ^= *s >> 12;
    *s ^= *s << 25;
    *s ^= *s >> 27;
    return *s * UINT64_C(2685821657736338717);
}

static inline uint64_t hq_seed_or_time(uint64_t seed)
{
    return seed != 0 ? seed : (hq_mono_ns() | 1u);
}

static int hq_cmp_double(const void *a, const void *b)
{
    const double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

/* Nearest-rank percentile; sorts v in place. */
static inline double hq_percentile(double *v, size_t n, double pct)
{
    size_t k;
    if (n == 0) return 0.0;
    qsort(v, n, sizeof(*v), hq_cmp_double);
    k = (size_t)((pct / 100.0) * (double)n + 0.999999);
    if (k < 1) k = 1;
    if (k > n) k = n;
    return v[k - 1];
}

static inline void hq_print_stats(const char *label, double *v, size_t n)
{
    /* Argument evaluation order is unspecified: read max only after the
     * sort, not in the same call (GCC took v[n-1] unsorted: max < p50). */
    const double p50 = hq_percentile(v, n, 50.0);
    const double p99 = hq_percentile(v, n, 99.0);
    printf("STATS %s n=%zu p50=%.3f p99=%.3f max=%.3f\n", label, n, p50, p99,
           n ? v[n - 1] : 0.0);
}

static inline int hq_fail(const char *what, int status)
{
    fprintf(stderr, "%s: %s (%d)\n", what, bladerf_strerror(status), status);
    return status != 0 ? status : BLADERF_ERR_UNEXPECTED;
}

static inline struct bladerf_rx_transition_request
hq_rx_request(uint64_t frequency_hz, unsigned timeout_ms)
{
    struct bladerf_rx_transition_request r = {
        .target_frequency_hz = frequency_hz,
        .required_events_mask = BLADERF_RF_REQUIRE_PLL_LOCKED |
                                BLADERF_RF_REQUIRE_ENSM_RX |
                                BLADERF_RF_REQUIRE_EPOCH_VALID,
        .timeout_ms = timeout_ms,
        .require_rx_data_valid = true,
        .epoch_settle_samples = 0,
    };
    return r;
}

/* Host-tuned transition to `frequency_hz`; fills the final epoch event. */
static inline int hq_transition(struct bladerf *dev, bladerf_channel ch,
                                uint64_t frequency_hz, unsigned timeout_ms,
                                struct bladerf_rf_event *final_event)
{
    const struct bladerf_rx_transition_request request =
        hq_rx_request(frequency_hz, timeout_ms);
    uint32_t txn = 0;
    int status = bladerf_rx_transition_begin(dev, ch, &request, &txn);
    if (status == 0)
        status = bladerf_rx_transition_wait(dev, txn, final_event,
                                            timeout_ms);
    return status;
}

/* Count RF events of two types after `*cursor` in ONE read; advances the
 * cursor. Two separate reads would lose the second type: the first read
 * already moved the cursor past the whole batch. */
static inline int hq_count_events2(struct bladerf *dev, uint64_t *cursor,
                                   bladerf_rf_event_type type_a,
                                   unsigned *count_a,
                                   bladerf_rf_event_type type_b,
                                   unsigned *count_b, bool *complete)
{
    struct bladerf_rf_event ev[BLADERF_RF_EVENT_HISTORY_SIZE];
    uint32_t n = 0;
    int status = bladerf_rf_events_get_since(
        dev, *cursor, ev, BLADERF_RF_EVENT_HISTORY_SIZE, &n, cursor,
        complete);
    for (uint32_t i = 0; status == 0 && i < n; ++i) {
        if (ev[i].event_type == type_a) ++*count_a;
        if (count_b != NULL && ev[i].event_type == type_b) ++*count_b;
    }
    return status;
}

/* Count RF events of one type after `*cursor`; advances the cursor. */
static inline int hq_count_events(struct bladerf *dev, uint64_t *cursor,
                                  bladerf_rf_event_type type,
                                  unsigned *count_type, bool *complete)
{
    return hq_count_events2(dev, cursor, type, count_type, type, NULL,
                            complete);
}

/* The FPGA RX trigger state outlives the process (the library never disarms
 * on open or close). An armed trigger gates RX samples, so an epoch waits in
 * WAIT_FIRST_SAMPLE until it times out. Returns 0 when RX is not armed. */
static inline int hq_require_trigger_disarmed(struct bladerf *dev)
{
    struct bladerf_trigger t;
    bool armed = false, fired = false, requested = false;
    int s = bladerf_trigger_init(dev, BLADERF_CHANNEL_RX(0),
                                 BLADERF_TRIGGER_MINI_EXP_1, &t);
    if (s == 0)
        s = bladerf_trigger_state(dev, &t, &armed, &fired, &requested, NULL,
                                  NULL);
    if (s != 0) return hq_fail("trigger state", s);
    if (armed) {
        fprintf(stderr, "NOT_EVALUATED: RX trigger is armed from an earlier "
                "run (run rx_trigger_live --disarm)\n");
        return BLADERF_ERR_INVAL;
    }
    return 0;
}

#endif
