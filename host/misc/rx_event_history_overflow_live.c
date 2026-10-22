/* RF event history overflow on a live xA4 (HW-FAULT-INJECTION-001).
 *
 * The native history keeps BLADERF_RF_EVENT_HISTORY_SIZE events. A reader
 * that falls further behind must be told, not handed a silently shortened
 * history. Run TRANSITIONS certified RX transitions without reading events,
 * then read from the cursor taken before them.
 * Usage: [transitions, default 24]
 * PASS (criteria fixed before the run):
 *   - more than BLADERF_RF_EVENT_HISTORY_SIZE events were produced
 *     (sequence advance), so the history really overflowed
 *   - that read returns BLADERF_ERR_MEM with history_complete=false and
 *     moves the cursor to the current sequence
 *   - the next read from the returned cursor is complete (status 0)
 *   - a fresh transition afterwards still certifies a new epoch and sync RX
 *     returns IQ of that epoch */
#include <libbladeRF.h>
#include "hwqual_live.h"

#include <unistd.h>

#define RX_CH BLADERF_CHANNEL_RX(0)
#define FREQ_A UINT64_C(1835000000)
#define FREQ_B UINT64_C(1835400000)
#define RATE 7680000u
#define BLOCK 8192u

static int seek_now(struct bladerf *dev, uint64_t *cursor)
{
    struct bladerf_rf_event ev[BLADERF_RF_EVENT_HISTORY_SIZE];
    uint32_t n = 0;
    bool complete = true;
    int s;
    /* Earlier history may already be gone: only the cursor matters here. */
    s = bladerf_rf_events_get_since(dev, *cursor, ev,
                                    BLADERF_RF_EVENT_HISTORY_SIZE, &n, cursor,
                                    &complete);
    return s == BLADERF_ERR_MEM ? 0 : s;
}

int main(int argc, char **argv)
{
    const unsigned transitions =
        argc > 1 ? (unsigned)strtoul(argv[1], NULL, 10) : 24;
    struct bladerf *dev = NULL;
    struct bladerf_rf_event ev[BLADERF_RF_EVENT_HISTORY_SIZE];
    struct bladerf_rf_event last = {0};
    uint64_t before = 0, after = 0, again = 0;
    uint32_t n = 0;
    bool complete = true, ok = false;
    int s, s_lost = 0;
    int16_t *buf = calloc(BLOCK * 2, sizeof(*buf));

    if (transitions == 0 || transitions > 1000 || buf == NULL) return 2;
    if ((s = bladerf_open(&dev, NULL)) != 0) {
        hq_fail("open", s);
        free(buf);
        return 1;
    }
    if ((s = bladerf_set_sample_rate(dev, RX_CH, RATE, NULL)) != 0 ||
        (s = bladerf_set_bandwidth(dev, RX_CH, RATE * 3 / 4, NULL)) != 0 ||
        (s = bladerf_set_frequency(dev, RX_CH, FREQ_A)) != 0 ||
        (s = bladerf_sync_config(dev, BLADERF_RX_X1,
                                 BLADERF_FORMAT_SC16_Q11_META, 32, BLOCK, 16,
                                 3000)) != 0 ||
        (s = bladerf_enable_module(dev, RX_CH, true)) != 0 ||
        (s = hq_transition(dev, RX_CH, FREQ_A, 3000, &last)) != 0) {
        hq_fail("configure", s);
        goto out;
    }
    if ((s = seek_now(dev, &before)) != 0) {
        hq_fail("seek", s);
        goto out;
    }

    for (unsigned i = 0; i < transitions; ++i) {
        s = hq_transition(dev, RX_CH, (i & 1u) ? FREQ_A : FREQ_B, 3000, &last);
        if (s != 0) {
            fprintf(stderr, "transition %u: %s\n", i, bladerf_strerror(s));
            goto out;
        }
    }

    after = before;
    s_lost = bladerf_rf_events_get_since(dev, before, ev,
                                         BLADERF_RF_EVENT_HISTORY_SIZE, &n,
                                         &after, &complete);
    printf("OVERFLOW_READ produced=%" PRIu64 " capacity=%u status=%d "
           "complete=%u returned=%u cursor_advance=%" PRIu64 "\n",
           after - before, (unsigned)BLADERF_RF_EVENT_HISTORY_SIZE, s_lost,
           complete, n, after - before);
    if (after - before <= BLADERF_RF_EVENT_HISTORY_SIZE) {
        fprintf(stderr, "history did not overflow; raise transitions\n");
        goto out;
    }
    if (s_lost != BLADERF_ERR_MEM || complete) {
        fprintf(stderr, "overflow not reported: status=%d complete=%u\n",
                s_lost, complete);
        goto out;
    }

    again = after;
    complete = false;
    s = bladerf_rf_events_get_since(dev, after, ev,
                                    BLADERF_RF_EVENT_HISTORY_SIZE, &n, &again,
                                    &complete);
    if (s != 0 || !complete) {
        fprintf(stderr, "read after overflow: status=%d complete=%u\n", s,
                complete);
        goto out;
    }

    s = hq_transition(dev, RX_CH, FREQ_B, 3000, &last);
    if (s != 0 || last.event_type != BLADERF_RF_EVT_RX_EPOCH_VALID) {
        fprintf(stderr, "transition after overflow: %s event=%u\n",
                bladerf_strerror(s), last.event_type);
        goto out;
    }
    for (unsigned attempt = 0; attempt < 40; ++attempt) {
        struct bladerf_metadata m = { .flags = BLADERF_META_FLAG_RX_NOW };
        s = bladerf_sync_rx(dev, buf, BLOCK, &m, 1000);
        if (s == BLADERF_ERR_WOULD_BLOCK) {
            usleep(25000);
            continue;
        }
        if (s != 0 || m.actual_count == 0 || !m.rx_epoch_id_valid ||
            m.rx_epoch_id != last.epoch_id) {
            fprintf(stderr, "IQ after overflow: %s count=%u epoch=%u/%u\n",
                    bladerf_strerror(s), m.actual_count, m.rx_epoch_id,
                    last.epoch_id);
            goto out;
        }
        ok = true;
        printf("IQ_AFTER_OVERFLOW epoch=%u count=%u\n", last.epoch_id,
               m.actual_count);
        break;
    }

out:
    if (dev != NULL) {
        (void)bladerf_enable_module(dev, RX_CH, false);
        bladerf_close(dev);
    }
    free(buf);
    printf("%s transitions=%u\n", ok ? "PASS" : "FAIL", transitions);
    return ok ? 0 : 1;
}
