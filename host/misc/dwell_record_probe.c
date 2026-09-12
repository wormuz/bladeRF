/* dwell_record_probe: proves the sweep revision's dwell_summary readout is
 * a real, live measurement, not stale/garbage register content.
 *
 * Uses the public bladerf_get_dwell_status/bladerf_get_dwell_summary/
 * bladerf_set_dwell_cfg API (host/libraries/libbladeRF/include/bladeRF2.h).
 *
 * Three checks, each independently falsifiable:
 *   1. GENERATION advances between reads (dwell boundaries are actually
 *      happening; a stuck generation means the block is not running).
 *   2. SAMPLE_COUNT is plausible: nonzero, not 0xFFFFFFFF.
 *   3. Energy responds to RX gain: read at 0 dB, then at 20 dB, same
 *      frequency -- energy_sum and mean_power must increase. If they do
 *      not, the block is returning constants, not a measurement.
 *
 * Also reported (not gating): clip_count at high gain vs 0 dB; the
 * noise_floor <= peak_window invariant, checked on every read.
 *
 *   cc -o dwell_record_probe dwell_record_probe.c -lbladeRF
 *   LD_PRELOAD=~/projects/bladerf/host/build/output/libbladeRF.so.2 \
 *     ./dwell_record_probe 925000000 2 5
 */
#include <libbladeRF.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>

/* Pull samples for approximately `secs` seconds so the RX datapath (and
 * therefore the dwell analyzer fed by it) has data flowing, then pulse
 * the SYNC_IN strobe low->high->low. bladerf_core.vhd starts a new dwell
 * on the RISING EDGE of this bit -- it is not automatic, the host must
 * drive it. Ignores sample content -- this probe only needs the stream
 * running, not the samples themselves. */
static void stream_and_dwell(struct bladerf *d, int16_t *buf, int secs)
{
    int iters = secs * 10 > 0 ? secs * 10 : 1;
    for (int i = 0; i < iters; i++) {
        int st = bladerf_sync_rx(d, buf, 8192, NULL, 1000);
        if (st) {
            printf("    bladerf_sync_rx: %s\n", bladerf_strerror(st));
        }
    }

    bladerf_set_dwell_sync(d, false);
    bladerf_set_dwell_sync(d, true);
    bladerf_sync_rx(d, buf, 8192, NULL, 1000); /* let the edge cross clock domains */
    bladerf_set_dwell_sync(d, false);
}

static void print_record(int idx, const struct bladerf_dwell_summary *s)
{
    printf("  [%d] gen=%u energy=%llu peak=%u clip=%u samples=%u "
           "mean_pow=%u floor=%llu peakwin=%llu\n",
           idx, s->generation,
           (unsigned long long)s->energy_sum, s->peak, s->clip_count,
           s->sample_count, s->mean_power,
           (unsigned long long)s->noise_floor,
           (unsigned long long)s->peak_window);
}

int main(int argc, char **argv)
{
    struct bladerf *d = NULL;
    int st;
    double freq = (argc > 1) ? atof(argv[1]) : 925000000.0;
    int secs    = (argc > 2) ? atoi(argv[2]) : 2;
    int nreads  = (argc > 3) ? atoi(argv[3]) : 5;

    bladerf_set_usb_reset_on_open(false);
    if ((st = bladerf_open(&d, NULL))) {
        printf("open: %s\n", bladerf_strerror(st));
        return 1;
    }

    bladerf_set_frequency(d, BLADERF_CHANNEL_RX(0), (uint64_t)freq);
    bladerf_set_sample_rate(d, BLADERF_CHANNEL_RX(0), 61440000, NULL);
    /* AGC is on by default; it would fight bladerf_set_gain below and
     * make the gain-response check meaningless. */
    bladerf_set_gain_mode(d, BLADERF_CHANNEL_RX(0), BLADERF_GAIN_MANUAL);

    /* The dwell analyzer is fed by the RX datapath (rx_clock/rx_reset in
     * bladerf_core.vhd): generation is stuck and every field reads 0 until
     * RX is actually streaming. Keep a stream running in the background
     * with periodic small pulls so dwells keep completing while we poll
     * the summary register. */
    bladerf_sync_config(d, BLADERF_RX_X1, BLADERF_FORMAT_SC16_Q11, 16, 8192, 8, 1000);
    bladerf_enable_module(d, BLADERF_CHANNEL_RX(0), true);
    int16_t *rxbuf = malloc(8192 * 2 * sizeof(int16_t));

    uint32_t dwell_status;
    st = bladerf_get_dwell_status(d, &dwell_status);
    if (st) {
        printf("bladerf_get_dwell_status: %s\n", bladerf_strerror(st));
        if (st == BLADERF_ERR_UNSUPPORTED) {
            printf("gateware does not implement the dwell analyzer "
                   "(version marker bits 31:28 read 0)\n");
        }
        bladerf_close(d);
        return 1;
    }
    printf("dwell status word: 0x%08x\n", dwell_status);

    bool gen_advanced = false;
    bool sample_count_ok = true;
    bool floor_invariant_ok = true;
    uint32_t first_gen = 0, last_gen = 0;
    struct bladerf_dwell_summary rec;

    printf("Phase 1: %d reads at %.0f Hz, 0 dB gain, ~%ds apart\n",
           nreads, freq, secs);
    bladerf_set_gain(d, BLADERF_CHANNEL_RX(0), 0);
    stream_and_dwell(d, rxbuf, 1); /* let gain settle, stream while doing it */

    for (int i = 0; i < nreads; i++) {
        st = bladerf_get_dwell_summary(d, &rec);
        if (st) {
            printf("bladerf_get_dwell_summary: %s\n", bladerf_strerror(st));
            bladerf_close(d);
            return 1;
        }
        print_record(i, &rec);

        if (i == 0) {
            first_gen = rec.generation;
        } else if (rec.generation != last_gen) {
            gen_advanced = true;
        }
        last_gen = rec.generation;

        if (rec.sample_count == 0 || rec.sample_count == 0xFFFFFFFFu) {
            sample_count_ok = false;
        }

        if (rec.noise_floor > rec.peak_window) {
            floor_invariant_ok = false;
            printf("    INVARIANT VIOLATION: floor=%llu > peakwin=%llu\n",
                   (unsigned long long)rec.noise_floor,
                   (unsigned long long)rec.peak_window);
        }

        if (i + 1 < nreads) stream_and_dwell(d, rxbuf, secs);
    }

    printf("Phase 2: gain response, same freq, 0 dB vs 20 dB\n");
    struct bladerf_dwell_summary low, high;

    bladerf_set_gain(d, BLADERF_CHANNEL_RX(0), 0);
    stream_and_dwell(d, rxbuf, 1);
    if ((st = bladerf_get_dwell_summary(d, &low))) {
        printf("bladerf_get_dwell_summary: %s\n", bladerf_strerror(st));
        bladerf_close(d);
        return 1;
    }
    print_record(-1, &low);

    bladerf_set_gain(d, BLADERF_CHANNEL_RX(0), 20);
    stream_and_dwell(d, rxbuf, 1);
    if ((st = bladerf_get_dwell_summary(d, &high))) {
        printf("bladerf_get_dwell_summary: %s\n", bladerf_strerror(st));
        bladerf_close(d);
        return 1;
    }
    print_record(-2, &high);

    bool energy_responds = (high.energy_sum > low.energy_sum) &&
                            (high.mean_power > low.mean_power);

    printf("  clip_count: 0dB=%u  20dB=%u\n", low.clip_count, high.clip_count);

    bladerf_enable_module(d, BLADERF_CHANNEL_RX(0), false);
    free(rxbuf);
    bladerf_close(d);

    printf("\nVERDICT\n");
    printf("  [%s] generation advances: first=%u last=%u advanced=%s\n",
           gen_advanced ? "PASS" : "FAIL",
           first_gen, last_gen, gen_advanced ? "yes" : "no");
    printf("  [%s] sample_count plausible: last=%u\n",
           sample_count_ok ? "PASS" : "FAIL", rec.sample_count);
    printf("  [%s] energy responds to gain: energy 0dB=%llu 20dB=%llu, "
           "mean_pow 0dB=%u 20dB=%u\n",
           energy_responds ? "PASS" : "FAIL",
           (unsigned long long)low.energy_sum,
           (unsigned long long)high.energy_sum,
           low.mean_power, high.mean_power);
    printf("  [%s] floor<=peakwin invariant held across phase 1 reads\n",
           floor_invariant_ok ? "PASS" : "FAIL");

    return (gen_advanced && sample_count_ok && energy_responds &&
            floor_invariant_ok) ? 0 : 1;
}
