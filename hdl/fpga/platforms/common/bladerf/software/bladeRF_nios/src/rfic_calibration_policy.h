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

#endif
