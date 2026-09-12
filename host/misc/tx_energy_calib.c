/* tx_energy_calib: sweeps TX1 gain in a TX1->RX1 loopback (30 dB pad in the
 * break) and reads the RX1 dwell_summary at each point, to calibrate
 * energy_sum/mean_power against a known relative TX power ramp.
 *
 * Loopback only -- both TX1 and RX1 are tuned to the same frequency and
 * TX1 output goes into the pad/loop, not an antenna.
 *
 *   cc -o tx_energy_calib tx_energy_calib.c -lbladeRF -lm
 *   LD_PRELOAD=~/projects/bladerf/host/build/output/libbladeRF.so.2 \
 *     ./tx_energy_calib 925000000
 */
#include <libbladeRF.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#define BUF_SAMPLES 8192

static void pulse_dwell(struct bladerf *d)
{
    bladerf_set_dwell_sync(d, false);
    bladerf_set_dwell_sync(d, true);
    bladerf_set_dwell_sync(d, false);
}

int main(int argc, char **argv)
{
    struct bladerf *d = NULL;
    int st;
    double freq = (argc > 1) ? atof(argv[1]) : 925000000.0;
    int sweep_lo = (argc > 2) ? atoi(argv[2]) : -999;
    int sweep_hi = (argc > 3) ? atoi(argv[3]) : -999;
    int sweep_step = (argc > 4) ? atoi(argv[4]) : 5;
    int fixed_gain_mode = (argc > 5) ? atoi(argv[5]) : 0; /* 1: hold gain=lo, repeat nreads times */

    bladerf_set_usb_reset_on_open(false);
    if ((st = bladerf_open(&d, NULL))) {
        printf("open: %s\n", bladerf_strerror(st)); return 1;
    }

    /* RX1 config: fixed manual gain so the sweep only sees TX power change. */
    bladerf_set_frequency(d, BLADERF_CHANNEL_RX(0), (uint64_t)freq);
    bladerf_set_sample_rate(d, BLADERF_CHANNEL_RX(0), 30720000, NULL);
    bladerf_set_gain_mode(d, BLADERF_CHANNEL_RX(0), BLADERF_GAIN_MANUAL);
    bladerf_set_gain(d, BLADERF_CHANNEL_RX(0), 10); /* fixed, see CLAUDE.md rule */

    /* TX1 config: same frequency, loopback pair. Combined RX+TX throughput
     * must stay under the board's ~80 Msps recommended max. */
    bladerf_set_frequency(d, BLADERF_CHANNEL_TX(0), (uint64_t)freq);
    bladerf_set_sample_rate(d, BLADERF_CHANNEL_TX(0), 30720000, NULL);

    const struct bladerf_range *txrange = NULL;
    bladerf_get_gain_range(d, BLADERF_CHANNEL_TX(0), &txrange);
    /* bladerf_range.min/max are in the RANGE'S NATIVE units; scale converts
     * native -> the caller-facing unit (dB for bladerf_set_gain). Divide by
     * scale to get dB bounds, or the sweep runs over raw register ticks. */
    double scale = txrange ? txrange->scale : 1.0;
    int gmin = txrange ? (int)llround(txrange->min * scale) : -20;
    int gmax = txrange ? (int)llround(txrange->max * scale) : 60;
    printf("TX1 gain range (native): [%lld, %lld] scale=%f -> dB [%d, %d]\n",
           txrange ? (long long)txrange->min : -1,
           txrange ? (long long)txrange->max : -1,
           scale, gmin, gmax);
    if (gmax - gmin > 200 || gmax < gmin) {
        printf("ABORT: implausible TX gain range in dB, refusing to sweep\n");
        bladerf_close(d);
        return 1;
    }

    bladerf_sync_config(d, BLADERF_RX_X1, BLADERF_FORMAT_SC16_Q11, 16, BUF_SAMPLES, 8, 1000);
    bladerf_sync_config(d, BLADERF_TX_X1, BLADERF_FORMAT_SC16_Q11, 16, BUF_SAMPLES, 8, 1000);
    bladerf_enable_module(d, BLADERF_CHANNEL_RX(0), true);
    bladerf_enable_module(d, BLADERF_CHANNEL_TX(0), true);

    /* Warm-up: the very first post-open gain/dwell reading is consistently
     * anomalous (huge energy_sum, nonzero clip) regardless of which gain
     * value it happens to be -- a transient from device open / gain-mode
     * switch, not a hardware cascade effect. Burn one throwaway reading at
     * the sweep's starting gain before recording anything. */
    {
        int16_t *warm_rx = malloc(BUF_SAMPLES * 2 * sizeof(int16_t));
        int16_t *warm_tx = malloc(BUF_SAMPLES * 2 * sizeof(int16_t));
        memset(warm_tx, 0, BUF_SAMPLES * 2 * sizeof(int16_t));
        for (int i = 0; i < BUF_SAMPLES; i++) { double ph = 2.0*M_PI*i/16.0; warm_tx[2*i] = (int16_t)(1800.0*cos(ph)); warm_tx[2*i+1] = (int16_t)(1800.0*sin(ph)); }
        for (int i = 0; i < 20; i++) {
            bladerf_sync_tx(d, warm_tx, BUF_SAMPLES, NULL, 1000);
            bladerf_sync_rx(d, warm_rx, BUF_SAMPLES, NULL, 1000);
        }
        free(warm_rx);
        free(warm_tx);
    }

    int16_t *rxbuf = malloc(BUF_SAMPLES * 2 * sizeof(int16_t));
    int16_t *txbuf = malloc(BUF_SAMPLES * 2 * sizeof(int16_t));
    /* A real tone, offset from the carrier.
     *
     * Constant I with Q=0 was tried first and is wrong here: it is DC in
     * baseband, so after the mixer it lands on the LO itself, where the
     * transmitter's own carrier leakage and DC offset correction live. The
     * measured response was a step rather than a proportional rise --
     * nothing until 60 dB, then x4500 in one 4 dB increment, and the step
     * moved when RX gain changed, which a real signal path would not do.
     *
     * A complex exponential at fs/16 puts the tone clear of the LO, so what
     * the analyser integrates is the tone and not a DC artefact. Amplitude
     * stays fixed across the sweep; only the TX gain register moves. */
    for (int i = 0; i < BUF_SAMPLES; i++) {
        double ph = 2.0 * M_PI * i / 16.0;
        txbuf[2 * i]     = (int16_t)(1800.0 * cos(ph));
        txbuf[2 * i + 1] = (int16_t)(1800.0 * sin(ph));
    }

    int lo = (sweep_lo != -999) ? sweep_lo : gmin;
    int hi = (sweep_hi != -999) ? sweep_hi : gmax;
    if (lo < gmin) lo = gmin;
    if (hi > gmax) hi = gmax;

    printf("\nnominal_gain_db,actual_gain_db,energy_sum,mean_power,peak,clip_count,sample_count\n");

    int npts = 0;
    long long energies[256];
    int gains[256];

    int reads_at_fixed = fixed_gain_mode ? (hi - lo) / (sweep_step ? sweep_step : 1) + 1 : 1;
    int fixed_g = lo;
    if (fixed_gain_mode) bladerf_set_gain(d, BLADERF_CHANNEL_TX(0), fixed_g);

    for (int rep = 0; rep < (fixed_gain_mode ? reads_at_fixed : 1); rep++) {
    for (int g = (fixed_gain_mode ? fixed_g : lo);
         g <= (fixed_gain_mode ? fixed_g : hi);
         g += sweep_step) {
        if (!fixed_gain_mode) bladerf_set_gain(d, BLADERF_CHANNEL_TX(0), g);

        int actual_native = 0;
        bladerf_get_gain(d, BLADERF_CHANNEL_TX(0), &actual_native);
        double actual_db = actual_native; /* bladerf_get_gain already returns dB */

        /* settle + keep both streams running so dwell keeps completing */
        for (int i = 0; i < 15; i++) {
            bladerf_sync_tx(d, txbuf, BUF_SAMPLES, NULL, 1000);
            bladerf_sync_rx(d, rxbuf, BUF_SAMPLES, NULL, 1000);
        }

        /* First dwell after a gain change is a transient (measured: energy
         * up to 4 orders of magnitude too high, nonzero clip, at a gain
         * where the settled reading has clip=0) -- burn one dwell/read
         * cycle and discard it before recording. Confirmed independent of
         * gain value: 8 repeated reads at a FIXED 52 dB gain showed the
         * same first-read spike, later reads flat at the true level. */
        for (int w = 0; w < 3; w++) {
            pulse_dwell(d);
            bladerf_sync_tx(d, txbuf, BUF_SAMPLES, NULL, 1000);
            bladerf_sync_rx(d, rxbuf, BUF_SAMPLES, NULL, 1000);
            struct bladerf_dwell_summary warm;
            bladerf_get_dwell_summary(d, &warm);
        }

        pulse_dwell(d);
        bladerf_sync_tx(d, txbuf, BUF_SAMPLES, NULL, 1000);
        bladerf_sync_rx(d, rxbuf, BUF_SAMPLES, NULL, 1000); /* cross clock domain */

        struct bladerf_dwell_summary rec;
        if ((st = bladerf_get_dwell_summary(d, &rec))) {
            printf("bladerf_get_dwell_summary: %s\n", bladerf_strerror(st));
            break;
        }

        printf("%d,%.3f,%llu,%u,%u,%u,%u\n", g, actual_db,
               (unsigned long long)rec.energy_sum, rec.mean_power, rec.peak,
               rec.clip_count, rec.sample_count);

        if (npts < 256) {
            energies[npts] = (long long)rec.energy_sum;
            gains[npts] = g;
            npts++;
        }
    }
    }

    bladerf_enable_module(d, BLADERF_CHANNEL_TX(0), false);
    bladerf_enable_module(d, BLADERF_CHANNEL_RX(0), false);
    free(rxbuf);
    free(txbuf);
    bladerf_close(d);

    /* slope check: 10*log10(energy) vs tx gain (dB), least-squares slope */
    if (npts >= 2) {
        double n = npts, sx = 0, sy = 0, sxx = 0, sxy = 0;
        for (int i = 0; i < npts; i++) {
            double x = gains[i];
            double y = energies[i] > 0 ? 10.0 * log10((double)energies[i]) : 0;
            sx += x; sy += y; sxx += x * x; sxy += x * y;
        }
        double slope = (n * sxy - sx * sy) / (n * sxx - sx * sx);
        printf("\nslope 10*log10(energy_sum) vs tx_gain_db: %.3f dB/dB "
               "(expect ~1.0 if energy_sum is linear power; ~2.0 would mean "
               "amplitude-like, not power)\n", slope);
    }

    return 0;
}
