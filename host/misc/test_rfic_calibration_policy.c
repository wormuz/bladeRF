#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "rfic_calibration_policy.h"

int main(void)
{
    const uint64_t calibrated_lo = 2400000000ULL;
    const uint64_t distant_lo = 70000000ULL;
    const uint64_t threshold = 100000000ULL;
    uint64_t history[RFIC_RX_RF_PORT_COUNT] = {0};

    history[0] = calibrated_lo;

    /* Ordinary large moves and an unknown baseline still require RFDC. */
    if (!rfic_rfdc_calibration_required(
            calibrated_lo, distant_lo, threshold) ||
        !rfic_rfdc_calibration_required(0, distant_lo, threshold)) {
        fputs("runtime or unknown-baseline RFDC requirement was suppressed\n",
              stderr);
        return 1;
    }

    /* UG-570 says RFDC is needed when the new carrier is more than 100 MHz
     * away. Match the host transition policy exactly at the boundary. */
    if (rfic_rfdc_port_calibration_required(
            history, 0, calibrated_lo + threshold - 1, threshold) ||
        rfic_rfdc_port_calibration_required(
            history, 0, calibrated_lo + threshold, threshold) ||
        !rfic_rfdc_port_calibration_required(
            history, 0, calibrated_lo + threshold + 1, threshold)) {
        fputs("NIOS RFDC threshold does not match strict >100 MHz rule\n",
              stderr);
        return 1;
    }

    return 0;
}
