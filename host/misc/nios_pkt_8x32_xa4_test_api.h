#ifndef NIOS_PKT_8X32_XA4_TEST_API_H_
#define NIOS_PKT_8X32_XA4_TEST_API_H_

#include <stdbool.h>
#include <stdint.h>

#include "devices.h"
#include "rffe_csr_policy.h"

#define PRETRIG_DEPTH 4096u

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
uint32_t loss_counter_half_read(loss_counter_half_t which);
uint32_t dwell_status_read(void);
uint32_t dwell_word_read(uint8_t word);
void dwell_cfg_write(uint32_t value);
uint32_t pretrig_read(uint16_t index);
void rf_link_cfg_set_speed(bool high_speed);
void rf_link_cfg_set_epoch_tag(uint8_t tag);
void rf_link_cfg_start(void);
void rf_link_cfg_stop(void);
void rf_link_cfg_clear_faults(void);
void rx_epoch_arm(uint8_t epoch_id);
void rx_epoch_complete(void);
void rx_epoch_abort(void);
uint32_t rx_epoch_status_read(void);
uint32_t rx_epoch_ts_lo_read(void);
uint32_t rx_epoch_ts_hi_read(void);
void tx_retune_gate_arm(uint64_t timestamp, bool immediate);
void tx_retune_gate_complete(void);
void tx_retune_gate_abort(void);
uint32_t tx_retune_gate_status_read(void);

#endif
