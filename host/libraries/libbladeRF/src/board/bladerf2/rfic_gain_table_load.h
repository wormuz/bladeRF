#ifndef BLADERF2_RFIC_GAIN_TABLE_LOAD_H_
#define BLADERF2_RFIC_GAIN_TABLE_LOAD_H_

#include <stdbool.h>
#include <stdint.h>

struct bladerf;

/* The RX gain table the ADI driver is loading right now on `dev`
 * (phy->gain_table_transaction), for the whole-table NIOS write
 * (NIOS_PKT_16x64_TARGET_GAIN_TABLE_REGION, RX-TRANSITION-1S3-001). `region`
 * is nios_pkt_retune2_gain_table_region() of the table's end frequency, the
 * key NIOS uses for its own copy. */
struct bladerf2_gain_table_load {
    uint8_t (*tab)[3];
    uint8_t rows;
    uint8_t lna;
    uint8_t region;
    bool split;
};

/* False: no gain-table load in progress (or no PHY). */
bool bladerf2_rfic_gain_table_load_current(struct bladerf *dev,
                                           struct bladerf2_gain_table_load *out);

#endif
