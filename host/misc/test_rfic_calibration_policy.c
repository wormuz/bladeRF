#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "rfic_calibration_policy.h"

int main(void)
{
    uint64_t history[RFIC_RX_RF_PORT_COUNT] = {0};

    /* An input without a resident result still requires RFDC. */
    if (!rfic_rfdc_port_calibration_required(history, 0) ||
        !rfic_rfdc_port_calibration_required(history, RFIC_RX_RF_PORT_COUNT) ||
        !rfic_rfdc_port_calibration_required(0, 0)) {
        fputs("unknown-baseline RFDC requirement was suppressed\n", stderr);
        return 1;
    }

    /* A calibrated input is not recalibrated for an LO move: 947.5 <->
     * 1835 MHz cost 0.7-1.36 s per crossing and tracking follows the LO. */
    rfic_rfdc_calibration_record(history, 1, UINT64_C(947500000));
    if (rfic_rfdc_port_calibration_required(history, 1)) {
        fputs("LO move on a calibrated input forced RFDC\n", stderr);
        return 1;
    }

    return 0;
}
