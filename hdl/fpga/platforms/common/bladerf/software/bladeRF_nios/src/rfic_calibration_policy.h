#ifndef BLADERF_NIOS_RFIC_CALIBRATION_POLICY_H_
#define BLADERF_NIOS_RFIC_CALIBRATION_POLICY_H_

#include <stdbool.h>
#include <stdint.h>

static inline void rfic_rfdc_calibration_history_reset(uint64_t *last_hz)
{
    if (last_hz != 0) {
        *last_hz = 0;
    }
}

static inline bool rfic_rfdc_calibration_required(uint64_t last_hz,
                                                  uint64_t requested_hz,
                                                  uint64_t threshold_hz)
{
    uint64_t delta_hz = requested_hz > last_hz
                            ? requested_hz - last_hz
                            : last_hz - requested_hz;

    return last_hz == 0 || delta_hz >= threshold_hz;
}

/* Cold RFIC initialization tunes through a temporary staging frequency
 * before returning to the LO at which ad9361_init() already completed RFDC
 * calibration. Do not recalibrate at that temporary point: it is never used
 * as an operating frequency, and the final tune returns to the calibrated
 * LO. Runtime tunes keep the ordinary distance-based policy. */
static inline bool rfic_rfdc_calibration_required_for_tune(
    uint64_t last_hz, uint64_t requested_hz, uint64_t threshold_hz,
    bool init_sequence, uint64_t staging_hz)
{
    if (init_sequence && last_hz != 0 && requested_hz == staging_hz) {
        return false;
    }

    return rfic_rfdc_calibration_required(last_hz, requested_hz,
                                          threshold_hz);
}

#endif
