/* RX trigger arm/fire/disarm cycle with the register master trigger
 * (BLADERF_TRIGGER_ROLE_MASTER on MINI_EXP_1; no external pin is driven by
 * this tool beyond the master configuration, and no slave is attached).
 *
 * Per trial: arm (revokes the RX epoch), begin an event-driven transition,
 * read trigger state, try one short sync RX (200 ms) and record whether IQ
 * arrived before the fire, fire, complete the transition, read one block,
 * read trigger state, disarm, read trigger state.
 *
 * Usage: [trials, default 20]
 * PASS (criteria fixed before the run):
 *   - state after arm: armed, not fired; after fire: has_fired; after
 *     disarm: not armed (any state-read failure is a FAIL)
 *   - the transition completes after the fire and its epoch is valid
 *   - the first post-fire block is full, epoch-valid with the transition
 *     epoch, no OVERRUN
 * Printed, not gated: pre_fire_iq (0/1 per trial and total), p50/p99/max of
 * fire-to-first-block latency (ms). Whether the gate withholds IQ before the
 * fire is a measurement here, not an assumption. */
#include <libbladeRF.h>
#include "hwqual_live.h"

#define RX_CH BLADERF_CHANNEL_RX(0)
#define RATE 3840000u
#define FREQ UINT64_C(1835000000)
#define BLOCK 8192u

static int state(struct bladerf *dev, const struct bladerf_trigger *t,
                 bool *armed, bool *fired)
{
    bool fire_requested = false;
    return bladerf_trigger_state(dev, t, armed, fired, &fire_requested, NULL,
                                 NULL);
}

int main(int argc, char **argv)
{
    const unsigned trials = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 10)
                                     : 20;
    struct bladerf *dev = NULL;
    struct bladerf_trigger trig;
    int16_t *buf = calloc(BLOCK * 2, sizeof(*buf));
    double *lat = calloc(trials ? trials : 1, sizeof(*lat));
    unsigned pre_fire_iq = 0, done = 0;
    int s;
    bool ok = true;

    if (trials == 0 || trials > 10000 || buf == NULL || lat == NULL) return 2;
    if ((s = bladerf_open(&dev, NULL)) != 0) {
        hq_fail("open", s);
        return 1;
    }
    if ((s = bladerf_set_sample_rate(dev, RX_CH, RATE, NULL)) != 0 ||
        (s = bladerf_set_bandwidth(dev, RX_CH, 1500000u, NULL)) != 0 ||
        (s = bladerf_set_frequency(dev, RX_CH, FREQ)) != 0 ||
        (s = bladerf_sync_config(dev, BLADERF_RX_X1,
                                 BLADERF_FORMAT_SC16_Q11_META, 16, BLOCK, 8,
                                 3000)) != 0 ||
        (s = bladerf_enable_module(dev, RX_CH, true)) != 0 ||
        (s = bladerf_trigger_init(dev, RX_CH, BLADERF_TRIGGER_MINI_EXP_1,
                                  &trig)) != 0) {
        hq_fail("configure", s);
        ok = false;
        goto out;
    }
    trig.role = BLADERF_TRIGGER_ROLE_MASTER;

    for (unsigned i = 0; ok && i < trials; ++i) {
        const struct bladerf_rx_transition_request rq =
            hq_rx_request(FREQ, 2000);
        struct bladerf_rf_event ev = {0};
        struct bladerf_metadata meta = { .flags = BLADERF_META_FLAG_RX_NOW };
        bool armed = false, fired = false;
        uint32_t txn = 0;
        uint64_t t_fire = 0;
        bool pre = false;

        s = bladerf_trigger_arm(dev, &trig, true, 0, 0);
        if (s == 0) s = state(dev, &trig, &armed, &fired);
        if (s != 0 || !armed || fired) {
            fprintf(stderr, "trial %u after arm: s=%s armed=%u fired=%u\n", i,
                    bladerf_strerror(s), armed, fired);
            ok = false;
            break;
        }
        s = bladerf_rx_transition_begin(dev, RX_CH, &rq, &txn);
        if (s == 0) {
            meta = (struct bladerf_metadata){ .flags =
                                              BLADERF_META_FLAG_RX_NOW };
            s = bladerf_sync_rx(dev, buf, BLOCK, &meta, 200);
            pre = (s == 0 && meta.actual_count > 0);
            if (s == BLADERF_ERR_TIMEOUT || s == BLADERF_ERR_WOULD_BLOCK)
                s = 0;
        }
        if (s == 0) {
            t_fire = hq_mono_ns();
            s = bladerf_trigger_fire(dev, &trig);
        }
        if (s == 0) s = state(dev, &trig, &armed, &fired);
        if (s != 0 || !fired) {
            fprintf(stderr, "trial %u after fire: s=%s fired=%u\n", i,
                    bladerf_strerror(s), fired);
            ok = false;
            break;
        }
        s = bladerf_rx_transition_wait(dev, txn, &ev, 2000);
        if (s == 0) {
            meta = (struct bladerf_metadata){ .flags =
                                              BLADERF_META_FLAG_RX_NOW };
            s = bladerf_sync_rx(dev, buf, BLOCK, &meta, 3000);
        }
        if (s != 0 || meta.actual_count != BLOCK ||
            (meta.status & BLADERF_META_STATUS_OVERRUN) ||
            !meta.rx_epoch_id_valid || meta.rx_epoch_id != ev.epoch_id) {
            fprintf(stderr, "trial %u post-fire block: s=%s count=%u "
                    "status=0x%x epoch=%u/%u valid=%u\n", i,
                    bladerf_strerror(s), meta.actual_count, meta.status,
                    meta.rx_epoch_id, ev.epoch_id, meta.rx_epoch_id_valid);
            ok = false;
            break;
        }
        lat[done++] = (hq_mono_ns() - t_fire) / 1e6;
        pre_fire_iq += pre;

        {
            struct bladerf_trigger off = trig;
            off.role = BLADERF_TRIGGER_ROLE_DISABLED;
            s = bladerf_trigger_arm(dev, &off, false, 0, 0);
            if (s == 0) s = state(dev, &off, &armed, &fired);
            if (s != 0 || armed) {
                fprintf(stderr, "trial %u after disarm: s=%s armed=%u\n", i,
                        bladerf_strerror(s), armed);
                ok = false;
                break;
            }
        }
        printf("trial=%u pre_fire_iq=%u epoch=%u\n", i, pre, ev.epoch_id);
    }
    if (done > 0) hq_print_stats("fire_to_first_block_ms", lat, done);
    printf("TOTAL trials=%u completed=%u pre_fire_iq=%u\n", trials, done,
           pre_fire_iq);
out:
    if (dev != NULL) {
        struct bladerf_trigger off = trig;
        off.role = BLADERF_TRIGGER_ROLE_DISABLED;
        (void)bladerf_trigger_arm(dev, &off, false, 0, 0);
        (void)bladerf_enable_module(dev, RX_CH, false);
        bladerf_close(dev);
    }
    free(buf);
    free(lat);
    printf("%s\n", ok && done == trials ? "PASS" : "FAIL");
    return ok && done == trials ? 0 : 1;
}
