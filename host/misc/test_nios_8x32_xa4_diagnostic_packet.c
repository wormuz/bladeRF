#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "devices.h"
#include "nios_pkt_8x32.h"
#include "pkt_8x32.h"
#include "pkt_retune2.h"

uint32_t control_reg_read(void) { return 0; }
bool control_reg_write(uint32_t value) { (void)value; return true; }
bool rffe_csr_read_checked(uint32_t *value)
{ if (value != NULL) *value = 0; return value != NULL; }
bool rffe_csr_write_checked(uint32_t value, uint32_t mask)
{ (void)value; (void)mask; return true; }
bool adf4351_write(uint32_t value) { (void)value; return true; }
uint32_t adf400x_spi_read(uint8_t addr) { return addr; }
bool adf400x_spi_write(uint32_t value) { (void)value; return true; }
bool adi_fastlock_save(bool is_tx, uint8_t rffe, uint16_t nios)
{ (void)is_tx; (void)rffe; (void)nios; return true; }
uint32_t pkt_retune2_failure_count(bladerf_module module)
{ return module == BLADERF_MODULE_RX ? 101 : 202; }
uint32_t pkt_retune2_success_count(bladerf_module module)
{ return module == BLADERF_MODULE_RX ? 303 : 404; }
bool pkt_retune2_completion_select(bladerf_module module, uint32_t sequence)
{ (void)module; (void)sequence; return true; }
uint32_t pkt_retune2_completion_word(uint8_t addr)
{ return UINT32_C(0xc0000000) | addr; }
uint32_t rf_link_status_read(void) { return 0; }
uint32_t rx_fault_causes_read(void) { return 0; }
uint32_t loss_counter_half_read(loss_counter_half_t which)
{ return UINT32_C(0xa0000000) | (uint32_t)which; }
uint32_t dwell_status_read(void) { return 0; }
uint32_t dwell_word_read(uint8_t word)
{ return UINT32_C(0xd0000000) | word; }
void dwell_cfg_write(uint32_t value) { (void)value; }
uint32_t pretrig_read(uint16_t index) { return index; }
void rf_link_cfg_set_speed(bool high_speed) { (void)high_speed; }
void rf_link_cfg_set_epoch_tag(uint8_t tag) { (void)tag; }
void rf_link_cfg_start(void) {}
void rf_link_cfg_stop(void) {}
void rf_link_cfg_clear_faults(void) {}
void rx_epoch_arm(uint8_t epoch_id) { (void)epoch_id; }
void rx_epoch_complete(void) {}
void rx_epoch_abort(void) {}
uint32_t rx_epoch_status_read(void) { return 0; }
uint32_t rx_epoch_ts_lo_read(void) { return 0; }
uint32_t rx_epoch_ts_hi_read(void) { return 0; }
void tx_retune_gate_arm(uint64_t timestamp, bool immediate)
{ (void)timestamp; (void)immediate; }
void tx_retune_gate_complete(void) {}
void tx_retune_gate_abort(void) {}
uint32_t tx_retune_gate_status_read(void) { return 0; }

#define CHECK_READ(target_value, addr_value, expected_success, expected_data) \
    do { \
        struct pkt_buf packet = { \
            .req = { NIOS_PKT_8x32_MAGIC, (target_value), 0, 0, \
                     (addr_value), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, \
            .resp = { 0 }, .ready = false, \
        }; \
        uint8_t target = 0, addr = 0; \
        uint32_t data = 0; \
        bool write = true, success = !(expected_success); \
        pkt_8x32(&packet); \
        nios_pkt_8x32_resp_unpack(packet.resp, &target, &write, &addr, \
                                  &data, &success); \
        if (target != (target_value) || write || addr != (addr_value) || \
            success != (expected_success) || data != (expected_data)) { \
            fprintf(stderr, "xA4 8x32 diagnostic failed: target=0x%02x " \
                    "addr=%u success=%u data=0x%08x\n", \
                    (unsigned)(target_value), (unsigned)(addr_value), \
                    (unsigned)success, (unsigned)data); \
            return 1; \
        } \
    } while (0)

int main(void)
{
    CHECK_READ(NIOS_PKT_8x32_TARGET_RETUNE2_FAILURES, 1, true, 202);
    CHECK_READ(NIOS_PKT_8x32_TARGET_RETUNE2_FAILURES, 2, false, 0);
    CHECK_READ(NIOS_PKT_8x32_TARGET_RETUNE2_SUCCESSES, 1, true, 404);
    CHECK_READ(NIOS_PKT_8x32_TARGET_RETUNE2_SUCCESSES, 2, false, 0);
    CHECK_READ(NIOS_PKT_8x32_TARGET_RETUNE2_COMPLETION, 5, true,
               UINT32_C(0xc0000005));
    CHECK_READ(NIOS_PKT_8x32_TARGET_RETUNE2_COMPLETION, 6, false, 0);
    CHECK_READ(NIOS_PKT_8x32_TARGET_LOSS_COUNTERS, 3, true,
               UINT32_C(0xa0000003));
    CHECK_READ(NIOS_PKT_8x32_TARGET_LOSS_COUNTERS, 4, false, 0);
    CHECK_READ(NIOS_PKT_8x32_TARGET_DWELL_READOUT, 15, true,
               UINT32_C(0xd000000f));
    CHECK_READ(NIOS_PKT_8x32_TARGET_DWELL_READOUT, 16, false, 0);

    puts("xA4 NIOS 8x32 diagnostic packet address status: PASS");
    return 0;
}
