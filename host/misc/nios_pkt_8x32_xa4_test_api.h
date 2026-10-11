#ifndef NIOS_PKT_8X32_XA4_TEST_API_H_
#define NIOS_PKT_8X32_XA4_TEST_API_H_

#include <stdbool.h>
#include <stdint.h>

#include "devices.h"
#include "rffe_csr_policy.h"

#define PRETRIG_DEPTH 4096u

#define RF_LINK_CFG_BASE 1u
#define RX_EPOCH_CTRL_BASE 2u
#define RX_EPOCH_STATUS_BASE 3u
#define RX_EPOCH_TS_LO_BASE 4u
#define RX_EPOCH_TS_HI_BASE 5u
#define TX_RETUNE_GATE_CTRL_BASE 6u
#define TX_RETUNE_GATE_TARGET_LO_BASE 7u
#define TX_RETUNE_GATE_TARGET_HI_BASE 8u
#define TX_RETUNE_GATE_STATUS_BASE 9u

uint32_t xa4_test_pio_read(uint32_t base);
void xa4_test_pio_write(uint32_t base, uint32_t value);
#define IORD_ALTERA_AVALON_PIO_DATA(base) xa4_test_pio_read(base)
#define IOWR_ALTERA_AVALON_PIO_DATA(base, value) \
    xa4_test_pio_write((base), (value))

typedef enum {
    LOSS_CNT_RX_LO = 0,
    LOSS_CNT_RX_HI = 1,
    LOSS_CNT_TX_LO = 2,
    LOSS_CNT_TX_HI = 3,
} loss_counter_half_t;

bool rffe_csr_read_checked(uint32_t *value);
bool rffe_csr_write_checked(uint32_t value, uint32_t output_mask);
uint32_t rf_link_status_read(void);
uint32_t rx_fault_causes_read(void);
bool loss_counter_half_read(loss_counter_half_t which, uint32_t *value);
uint32_t dwell_status_read(void);
uint32_t dwell_word_read(uint8_t word);
bool dwell_cfg_write(uint32_t value);
uint32_t pretrig_read(uint16_t index);
bool rf_link_cfg_set_speed(bool high_speed);
bool rf_link_cfg_set_epoch_tag(uint8_t tag);
bool rf_link_cfg_start(void);
bool rf_link_cfg_stop(void);
bool rf_link_cfg_clear_faults(void);
bool rx_epoch_arm(uint8_t epoch_id);
bool rx_epoch_complete(void);
bool rx_epoch_abort(void);
uint32_t rx_epoch_status_read(void);
uint32_t rx_epoch_ts_lo_read(void);
uint32_t rx_epoch_ts_hi_read(void);
bool tx_retune_gate_arm(uint64_t timestamp, bool immediate);
bool tx_retune_gate_complete(void);
bool tx_retune_gate_abort(void);
uint32_t tx_retune_gate_status_read(void);

#endif
