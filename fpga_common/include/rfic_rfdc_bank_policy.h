#ifndef BLADERF_RFIC_RFDC_BANK_POLICY_H_
#define BLADERF_RFIC_RFDC_BANK_POLICY_H_

#include <stdint.h>

/* AD9361 RX RF input selectors as returned by ad9361_get_rx_rf_port_input()
 * for the balanced inputs bladeRF 2 uses. Shared by NIOS and libbladeRF so
 * both RFDC history caches apply the same bank rule. */
#define RFIC_RFDC_RX_PORT_A 0u
#define RFIC_RFDC_RX_PORT_B 1u
#define RFIC_RFDC_RX_PORT_C 2u
#define RFIC_RFDC_RX_PORT_COUNT 3u

/* UG-570 keeps a separate RFDC result bank for input A and one bank shared by
 * inputs B and C. A calibration on B or C overwrites that shared bank, so the
 * peer selector's history no longer proves its correction is resident.
 * Returns the peer selector, or RFIC_RFDC_RX_PORT_COUNT when none exists. */
static inline uint32_t rfic_rfdc_rx_bank_peer(uint32_t port)
{
    if (port == RFIC_RFDC_RX_PORT_B) {
        return RFIC_RFDC_RX_PORT_C;
    }
    if (port == RFIC_RFDC_RX_PORT_C) {
        return RFIC_RFDC_RX_PORT_B;
    }
    return RFIC_RFDC_RX_PORT_COUNT;
}

#endif
