#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "devices.h"
#include "nios_pkt_8x32.h"
#include "nios_ram_watermark.h"
#include "pkt_8x32.h"
#include "pkt_retune2.h"

static unsigned int control_write_calls;
static unsigned int rffe_write_calls;
static unsigned int adf4351_write_calls;
uint32_t control_reg_read(void) { return 0; }
bool control_reg_write(uint32_t value)
{ (void)value; control_write_calls++; return true; }
bool rffe_csr_read_checked(uint32_t *value)
{ if (value != NULL) *value = 0; return value != NULL; }
bool rffe_csr_write_checked(uint32_t value, uint32_t mask)
{ (void)value; (void)mask; rffe_write_calls++; return true; }
bool adf4351_write(uint32_t value)
{ (void)value; adf4351_write_calls++; return true; }
uint32_t adf400x_spi_read(uint8_t addr) { return addr; }
static unsigned int adf400x_write_calls;
bool adf400x_spi_write(uint32_t value)
{ (void)value; adf400x_write_calls++; return true; }
static unsigned int fastlock_save_calls;
static bool fastlock_save_success = true;
static bool fastlock_save_last_tx;
static uint8_t fastlock_save_last_rffe;
static uint16_t fastlock_save_last_nios;
bool adi_fastlock_save(bool is_tx, uint8_t rffe, uint16_t nios)
{
    fastlock_save_calls++;
    fastlock_save_last_tx = is_tx;
    fastlock_save_last_rffe = rffe;
    fastlock_save_last_nios = nios;
    return fastlock_save_success;
}
uint32_t pkt_retune2_failure_count(bladerf_module module)
{ return module == BLADERF_MODULE_RX ? 101 : 202; }
uint32_t pkt_retune2_success_count(bladerf_module module)
{ return module == BLADERF_MODULE_RX ? 303 : 404; }
bool pkt_retune2_completion_select(bladerf_module module, uint32_t sequence)
{ (void)module; (void)sequence; return true; }
uint32_t pkt_retune2_completion_word(uint8_t addr)
{ return UINT32_C(0xc0000000) | addr; }
bool pkt_retune2_failure_completion_select(bladerf_module module,
                                           uint32_t sequence)
{ (void)module; (void)sequence; return true; }
uint32_t rf_link_status_read(void) { return 0; }
uint32_t rx_fault_causes_read(void) { return 0; }
static bool loss_counter_available = true;
bool loss_counter_half_read(loss_counter_half_t which, uint32_t *value)
{
    if (!loss_counter_available || value == NULL) {
        return false;
    }
    *value = UINT32_C(0xa0000000) | (uint32_t)which;
    return true;
}
uint32_t dwell_status_read(void) { return 0; }
static unsigned int nios_ram_read_calls;
bool nios_ram_watermark_read(uint8_t field, uint32_t *bytes)
{
    nios_ram_read_calls++;
    *bytes = UINT32_C(0x5a000000) | field;
    return true;
}
static unsigned int i2c_diag_read_calls;
bool i2c_diag_read(uint8_t addr, uint32_t *value)
{
    i2c_diag_read_calls++;
    if (addr > NIOS_PKT_8x32_I2C_DIAG_RECOVERY) {
        return false;
    }
    *value = UINT32_C(0x12c00000) | addr;
    return true;
}
uint32_t dwell_word_read(uint8_t word)
{ return UINT32_C(0xd0000000) | word; }
uint32_t command_uart_current_session_generation(void) { return 0; }
static bool dwell_cfg_success;
static unsigned int dwell_cfg_calls;
bool dwell_cfg_write(uint32_t value)
{
    (void)value;
    dwell_cfg_calls++;
    return dwell_cfg_success;
}
uint32_t pretrig_read(uint16_t index) { return index; }
static bool gate_command_success;
static unsigned int gate_fifo_flush_calls;
static unsigned int gate_fifo_release_calls;
static bool pio_write_success = true;
static uint32_t pio_values[10];
uint32_t xa4_test_pio_read(uint32_t base)
{
    return base < sizeof(pio_values) / sizeof(pio_values[0])
               ? pio_values[base]
               : 0;
}
void xa4_test_pio_write(uint32_t base, uint32_t value)
{
    if (pio_write_success &&
        base < sizeof(pio_values) / sizeof(pio_values[0])) {
        pio_values[base] = value;
    }
}
bool rf_link_cfg_set_speed(bool high_speed)
{ (void)high_speed; return gate_command_success; }
bool rf_link_cfg_set_epoch_tag(uint8_t tag)
{ (void)tag; return gate_command_success; }
bool rf_link_cfg_start(void) { return gate_command_success; }
bool rf_link_cfg_stop(void) { return gate_command_success; }
bool rf_link_cfg_clear_faults(void) { return gate_command_success; }
bool rx_epoch_arm(uint8_t epoch_id)
{ (void)epoch_id; return gate_command_success; }
bool rx_epoch_complete(void) { return gate_command_success; }
bool rx_epoch_abort(void) { return gate_command_success; }
uint32_t rx_epoch_status_read(void) { return 0; }
uint32_t rx_epoch_ts_lo_read(void) { return 0; }
uint32_t rx_epoch_ts_hi_read(void) { return 0; }
bool tx_retune_gate_arm(uint64_t timestamp, bool immediate)
{ (void)timestamp; (void)immediate; return gate_command_success; }
bool tx_retune_gate_complete(void) { return gate_command_success; }
bool tx_retune_gate_abort(void) { return gate_command_success; }
bool tx_retune_gate_fifo_flush(void)
{ gate_fifo_flush_calls++; return gate_command_success; }
bool tx_retune_gate_fifo_release(void)
{ gate_fifo_release_calls++; return gate_command_success; }
uint32_t tx_retune_gate_status_read(void) { return 0; }
static bool sfir_success = true;
static unsigned int sfir_write_calls;
static unsigned int sfir_commit_calls;
static unsigned int sfir_identity_calls;
static uint8_t sfir_last_tag;
uint32_t rx_sfir_status_read(void) { return UINT32_C(0x1003a5e3); }
bool rx_sfir_write_word(uint8_t idx, uint32_t word)
{ (void)idx; (void)word; sfir_write_calls++; return sfir_success; }
bool rx_sfir_commit(uint8_t tag)
{ sfir_last_tag = tag; sfir_commit_calls++; return sfir_success; }
bool rx_sfir_identity(void) { sfir_identity_calls++; return sfir_success; }

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

#define CHECK_WRITE(target_value, addr_value, data_value, expected_success) \
    do { \
        uint8_t packed[NIOS_PKT_LEN]; \
        struct pkt_buf packet = { \
            .req = { NIOS_PKT_8x32_MAGIC, (target_value), \
                     NIOS_PKT_8x32_FLAG_WRITE, 0, (addr_value), \
                     (uint8_t)(data_value), (uint8_t)((data_value) >> 8), \
                     (uint8_t)((data_value) >> 16), \
                     (uint8_t)((data_value) >> 24), \
                     0, 0, 0, 0, 0, 0, 0 }, \
            .resp = { 0 }, .ready = false, \
        }; \
        uint8_t target = 0, addr = 0; \
        uint32_t data = 0; \
        bool write = false, success = !(expected_success); \
        nios_pkt_8x32_pack(packed, (target_value), true, (addr_value), \
                           (data_value)); \
        if (memcmp(packed, packet.req, sizeof(packed)) != 0) { \
            fputs("xA4 8x32 request packing changed", stderr); \
            return 1; \
        } \
        pkt_8x32(&packet); \
        nios_pkt_8x32_resp_unpack(packet.resp, &target, &write, &addr, \
                                  &data, &success); \
        if (target != (target_value) || !write || addr != (addr_value) || \
            success != (expected_success)) { \
            fprintf(stderr, "xA4 8x32 capability write failed: " \
                    "target=0x%02x addr=%u success=%u data=0x%08x\n", \
                    (unsigned)(target_value), (unsigned)(addr_value), \
                    (unsigned)success, (unsigned)data); \
            return 1; \
        } \
    } while (0)

static int check_single_register_write_rejected(uint8_t target, uint8_t addr)
{
    struct pkt_buf packet = {
        .req = { NIOS_PKT_8x32_MAGIC, target, NIOS_PKT_8x32_FLAG_WRITE,
                 0, addr, 0x78, 0x56, 0x34, 0x12, 0, 0, 0, 0, 0, 0, 0 },
        .resp = { 0 }, .ready = false,
    };
    unsigned int control_before = control_write_calls;
    unsigned int rffe_before = rffe_write_calls;
    unsigned int adf4351_before = adf4351_write_calls;
    uint8_t response_target = 0, response_addr = 0;
    uint32_t response_data = 0;
    bool response_write = false, success = true;

    pkt_8x32(&packet);
    nios_pkt_8x32_resp_unpack(packet.resp, &response_target,
                              &response_write, &response_addr,
                              &response_data, &success);
    return response_target == target && response_write &&
           response_addr == addr && !success &&
           control_write_calls == control_before &&
           rffe_write_calls == rffe_before &&
           adf4351_write_calls == adf4351_before ? 0 : 1;
}

int main(void)
{
    if (!nios_pkt_8x32_rfic_owner_access_safe(
            NIOS_PKT_8x32_TARGET_RETUNE2_SUCCESSES, false, 0, 0) ||
        !nios_pkt_8x32_rfic_owner_access_safe(
            NIOS_PKT_8x32_TARGET_RETUNE2_FAILURES, false, 0, 0) ||
        !nios_pkt_8x32_rfic_owner_access_safe(
            NIOS_PKT_8x32_TARGET_RETUNE2_COMPLETION, false, 0, 0) ||
        !nios_pkt_8x32_rfic_owner_access_safe(
            NIOS_PKT_8x32_TARGET_RETUNE2_COMPLETION, true, 0, 0) ||
        nios_pkt_8x32_rfic_owner_access_safe(
            NIOS_PKT_8x32_TARGET_RETUNE2_SUCCESSES, true, 0, 0) ||
        nios_pkt_8x32_rfic_owner_access_safe(
            NIOS_PKT_8x32_TARGET_RETUNE2_FAILURES, true, 0, 0) ||
        nios_pkt_8x32_rfic_owner_access_safe(
            NIOS_PKT_8x32_TARGET_CONTROL, false, 0, 0) ||
        !nios_pkt_8x32_rfic_owner_access_safe(
            NIOS_PKT_8x32_TARGET_RF_LINK_STATUS, false, 0, 0) ||
        !nios_pkt_8x32_rfic_owner_access_safe(
            NIOS_PKT_8x32_TARGET_LOSS_COUNTERS, false, 3, 0) ||
        !nios_pkt_8x32_rfic_owner_access_safe(
            NIOS_PKT_8x32_TARGET_NIOS_RAM, false,
            NIOS_PKT_8x32_NIOS_RAM_UNTOUCHED, 0) ||
        !nios_pkt_8x32_rfic_owner_access_safe(
            NIOS_PKT_8x32_TARGET_I2C_DIAG, false,
            NIOS_PKT_8x32_I2C_DIAG_REGS, 0) ||
        !nios_pkt_8x32_rfic_owner_access_safe(
            NIOS_PKT_8x32_TARGET_RX_EPOCH_STATUS, false, 0, 0) ||
        !nios_pkt_8x32_rfic_owner_access_safe(
            NIOS_PKT_8x32_TARGET_TX_RETUNE_GATE, false,
            NIOS_PKT_8x32_TX_RETUNE_CMD_STATUS, 0) ||
        !nios_pkt_8x32_rfic_owner_access_safe(
            NIOS_PKT_8x32_TARGET_TX_RETUNE_GATE, true,
            NIOS_PKT_8x32_TX_RETUNE_CMD_ABORT, 0) ||
        !nios_pkt_8x32_rfic_owner_access_safe(
            NIOS_PKT_8x32_TARGET_TX_RETUNE_GATE, true,
            NIOS_PKT_8x32_TX_RETUNE_CMD_COMPLETE, 0) ||
        nios_pkt_8x32_rfic_owner_access_safe(
            NIOS_PKT_8x32_TARGET_TX_RETUNE_GATE, true,
            NIOS_PKT_8x32_TX_RETUNE_CMD_ARM, 0) ||
        !nios_pkt_8x32_rfic_owner_access_safe(
            NIOS_PKT_8x32_TARGET_RFFE_CSR, false, 0, 0) ||
        nios_pkt_8x32_rfic_owner_access_safe(
            NIOS_PKT_8x32_TARGET_RFFE_CSR, true, 0, 0) ||
        !nios_pkt_8x32_rfic_owner_access_safe(
            NIOS_PKT_8x32_TARGET_COMMAND_SESSION_GENERATION, false, 0, 0) ||
        nios_pkt_8x32_rfic_owner_access_safe(
            NIOS_PKT_8x32_TARGET_NIOS_RAM, true, 0, 0) ||
        !nios_pkt_8x32_rfic_owner_access_safe(
            NIOS_PKT_8x32_TARGET_RX_SPATIAL_FIR, false,
            NIOS_PKT_8x32_SFIR_ADDR_STATUS, 0) ||
        !nios_pkt_8x32_rfic_owner_access_safe(
            NIOS_PKT_8x32_TARGET_RX_SPATIAL_FIR, true,
            NIOS_PKT_8x32_SFIR_CMD_IDENTITY, 0) ||
        nios_pkt_8x32_rfic_owner_access_safe(
            NIOS_PKT_8x32_TARGET_RX_SPATIAL_FIR, true,
            NIOS_PKT_8x32_SFIR_CMD_IDENTITY, 1) ||
        nios_pkt_8x32_rfic_owner_access_safe(
            NIOS_PKT_8x32_TARGET_RX_SPATIAL_FIR, true,
            NIOS_PKT_8x32_SFIR_CMD_COMMIT, 1) ||
        nios_pkt_8x32_rfic_owner_access_safe(
            NIOS_PKT_8x32_TARGET_RX_SPATIAL_FIR, true, 0, 0)) {
        fputs("RFIC-owner 8x32 access policy mismatch\n", stderr);
        return 1;
    }

    CHECK_READ(NIOS_PKT_8x32_TARGET_VERSION, 1, false, 0);
    CHECK_READ(NIOS_PKT_8x32_TARGET_CONTROL, 1, false, 0);
    CHECK_READ(NIOS_PKT_8x32_TARGET_RFFE_CSR, 1, false, 0);
    CHECK_READ(NIOS_PKT_8x32_TARGET_RETUNE2_FAILURES, 1, true, 202);
    CHECK_READ(NIOS_PKT_8x32_TARGET_RETUNE2_FAILURES, 2, false, 0);
    CHECK_READ(NIOS_PKT_8x32_TARGET_RETUNE2_SUCCESSES, 1, true, 404);
    CHECK_READ(NIOS_PKT_8x32_TARGET_RETUNE2_SUCCESSES, 2, false, 0);
    CHECK_READ(NIOS_PKT_8x32_TARGET_RETUNE2_COMPLETION,
               NIOS_PKT_8x32_RETUNE2_COMPLETION_REQUEST_TOKEN, true,
               UINT32_C(0xc0000000) |
                   NIOS_PKT_8x32_RETUNE2_COMPLETION_REQUEST_TOKEN);
    CHECK_READ(NIOS_PKT_8x32_TARGET_RETUNE2_COMPLETION,
               NIOS_PKT_8x32_RETUNE2_COMPLETION_REQUEST_TOKEN + 1, false,
               0);
    CHECK_READ(NIOS_PKT_8x32_TARGET_LOSS_COUNTERS, 3, true,
               UINT32_C(0xa0000003));
    CHECK_READ(NIOS_PKT_8x32_TARGET_LOSS_COUNTERS, 4, false, 0);
    loss_counter_available = false;
    CHECK_READ(NIOS_PKT_8x32_TARGET_LOSS_COUNTERS, 0, false, 0);
    loss_counter_available = true;
    CHECK_READ(NIOS_PKT_8x32_TARGET_DWELL_READOUT, 15, true,
               UINT32_C(0xd000000f));
    CHECK_READ(NIOS_PKT_8x32_TARGET_DWELL_READOUT, 16, false, 0);
    CHECK_READ(NIOS_PKT_8x32_TARGET_ADF400X, 3, true, 3);
    CHECK_READ(NIOS_PKT_8x32_TARGET_ADF400X, 4, false, 0);
    CHECK_WRITE(NIOS_PKT_8x32_TARGET_ADF400X, 0, 3, true);
    CHECK_WRITE(NIOS_PKT_8x32_TARGET_ADF400X, 1, 3, false);
    if (adf400x_write_calls != 1) {
        fputs("ADF400X accepted reserved write address\n", stderr);
        return 1;
    }
    CHECK_READ(NIOS_PKT_8x32_TARGET_RF_LINK_STATUS, 0, true, 0);
    CHECK_READ(NIOS_PKT_8x32_TARGET_RF_LINK_STATUS, 1, false, 0);
    CHECK_READ(NIOS_PKT_8x32_TARGET_RX_FAULT_CAUSES, 0, true, 0);
    CHECK_READ(NIOS_PKT_8x32_TARGET_RX_FAULT_CAUSES, 1, false, 0);
    CHECK_READ(NIOS_PKT_8x32_TARGET_DWELL_STATUS, 0, true, 0);
    CHECK_READ(NIOS_PKT_8x32_TARGET_DWELL_STATUS, 1, false, 0);
    CHECK_READ(NIOS_PKT_8x32_TARGET_NIOS_RAM,
               NIOS_PKT_8x32_NIOS_RAM_REGION, true,
               UINT32_C(0x5a000000) | NIOS_PKT_8x32_NIOS_RAM_REGION);
    CHECK_READ(NIOS_PKT_8x32_TARGET_NIOS_RAM,
               NIOS_PKT_8x32_NIOS_RAM_REGION + 1, false, 0);
    if (nios_ram_read_calls != 1) {
        fputs("NIOS RAM read reached the firmware for a reserved field\n",
              stderr);
        return 1;
    }
    CHECK_READ(NIOS_PKT_8x32_TARGET_I2C_DIAG,
               NIOS_PKT_8x32_I2C_DIAG_RECOVERY, true,
               UINT32_C(0x12c00000) | NIOS_PKT_8x32_I2C_DIAG_RECOVERY);
    CHECK_READ(NIOS_PKT_8x32_TARGET_I2C_DIAG,
               NIOS_PKT_8x32_I2C_DIAG_RECOVERY + 1, false, 0);
    if (i2c_diag_read_calls != 1) {
        fputs("I2C diagnostic read reached the firmware for a reserved "
              "field\n", stderr);
        return 1;
    }
    CHECK_READ(NIOS_PKT_8x32_TARGET_RX_EPOCH_STATUS, 0, true, 0);
    CHECK_READ(NIOS_PKT_8x32_TARGET_RX_EPOCH_STATUS, 1, false, 0);
    CHECK_READ(NIOS_PKT_8x32_TARGET_RX_EPOCH_TS_LO, 0, true, 0);
    CHECK_READ(NIOS_PKT_8x32_TARGET_RX_EPOCH_TS_LO, 1, false, 0);
    CHECK_READ(NIOS_PKT_8x32_TARGET_RX_EPOCH_TS_HI, 0, true, 0);
    CHECK_READ(NIOS_PKT_8x32_TARGET_RX_EPOCH_TS_HI, 1, false, 0);
    CHECK_READ(NIOS_PKT_8x32_TARGET_TX_RETUNE_GATE,
               NIOS_PKT_8x32_TX_RETUNE_CMD_STATUS, true, 0);
    CHECK_READ(NIOS_PKT_8x32_TARGET_TX_RETUNE_GATE,
               NIOS_PKT_8x32_TX_RETUNE_CMD_STATUS + 1, false, 0);
    if (check_single_register_write_rejected(
            NIOS_PKT_8x32_TARGET_CONTROL, 1) ||
        check_single_register_write_rejected(
            NIOS_PKT_8x32_TARGET_ADF4351, 1) ||
        check_single_register_write_rejected(
            NIOS_PKT_8x32_TARGET_RFFE_CSR, 1)) {
        fputs("single-register packet accepted a nonzero address\n", stderr);
        return 1;
    }
    gate_command_success = true;
    CHECK_WRITE(NIOS_PKT_8x32_TARGET_RX_EPOCH_CTRL,
                NIOS_PKT_8x32_RX_EPOCH_CMD_ARM, 7, true);
    CHECK_WRITE(NIOS_PKT_8x32_TARGET_RX_EPOCH_CTRL,
                NIOS_PKT_8x32_RX_EPOCH_CMD_COMPLETE, 0, true);
    CHECK_WRITE(NIOS_PKT_8x32_TARGET_RX_EPOCH_CTRL,
                NIOS_PKT_8x32_RX_EPOCH_CMD_ABORT, 0, true);
    CHECK_WRITE(NIOS_PKT_8x32_TARGET_RX_EPOCH_CTRL,
                NIOS_PKT_8x32_RX_EPOCH_CMD_ARM, UINT32_C(0x101), false);
    CHECK_WRITE(NIOS_PKT_8x32_TARGET_RX_EPOCH_CTRL,
                NIOS_PKT_8x32_RX_EPOCH_CMD_COMPLETE, 1, false);
    CHECK_WRITE(NIOS_PKT_8x32_TARGET_RX_EPOCH_CTRL,
                NIOS_PKT_8x32_RX_EPOCH_CMD_ABORT, 1, false);
    CHECK_WRITE(NIOS_PKT_8x32_TARGET_RF_LINK_CFG,
                NIOS_PKT_8x32_RF_LINK_CMD_START, 0, true);
    CHECK_WRITE(NIOS_PKT_8x32_TARGET_RF_LINK_CFG,
                NIOS_PKT_8x32_RF_LINK_CMD_SET_SPEED, 1, true);
    CHECK_WRITE(NIOS_PKT_8x32_TARGET_RF_LINK_CFG,
                NIOS_PKT_8x32_RF_LINK_CMD_SET_SPEED, 2, false);
    CHECK_WRITE(NIOS_PKT_8x32_TARGET_RF_LINK_CFG,
                NIOS_PKT_8x32_RF_LINK_CMD_SET_TAG, 7, true);
    CHECK_WRITE(NIOS_PKT_8x32_TARGET_RF_LINK_CFG,
                NIOS_PKT_8x32_RF_LINK_CMD_SET_TAG, UINT32_C(0x100), false);
    CHECK_WRITE(NIOS_PKT_8x32_TARGET_RF_LINK_CFG,
                NIOS_PKT_8x32_RF_LINK_CMD_START, 1, false);
    CHECK_WRITE(NIOS_PKT_8x32_TARGET_RF_LINK_CFG,
                NIOS_PKT_8x32_RF_LINK_CMD_STOP, 1, false);
    CHECK_WRITE(NIOS_PKT_8x32_TARGET_RF_LINK_CFG,
                NIOS_PKT_8x32_RF_LINK_CMD_CLEAR_FAULTS, 1, false);
    CHECK_WRITE(NIOS_PKT_8x32_TARGET_RF_LINK_CFG,
                NIOS_PKT_8x32_RF_LINK_CMD_STOP, 0, true);
    CHECK_WRITE(NIOS_PKT_8x32_TARGET_RF_LINK_CFG,
                NIOS_PKT_8x32_RF_LINK_CMD_CLEAR_FAULTS, 0, true);
    gate_command_success = false;
    CHECK_WRITE(NIOS_PKT_8x32_TARGET_RX_EPOCH_CTRL,
                NIOS_PKT_8x32_RX_EPOCH_CMD_ARM, 7, false);
    CHECK_WRITE(NIOS_PKT_8x32_TARGET_RF_LINK_CFG,
                NIOS_PKT_8x32_RF_LINK_CMD_START, 0, false);
    pio_write_success = true;
    CHECK_WRITE(NIOS_PKT_8x32_TARGET_TX_RETUNE_GATE,
                NIOS_PKT_8x32_TX_RETUNE_CMD_TARGET_LO,
                UINT32_C(0x12345678), true);
    pio_write_success = false;
    CHECK_WRITE(NIOS_PKT_8x32_TARGET_TX_RETUNE_GATE,
                NIOS_PKT_8x32_TX_RETUNE_CMD_TARGET_HI,
                UINT32_C(0xabcdef01), false);
    pio_write_success = true;
    gate_command_success = true;
    CHECK_WRITE(NIOS_PKT_8x32_TARGET_TX_RETUNE_GATE,
                NIOS_PKT_8x32_TX_RETUNE_CMD_ARM, 1, true);
    CHECK_WRITE(NIOS_PKT_8x32_TARGET_TX_RETUNE_GATE,
                NIOS_PKT_8x32_TX_RETUNE_CMD_ARM, 2, false);
    gate_command_success = false;
    CHECK_WRITE(NIOS_PKT_8x32_TARGET_TX_RETUNE_GATE,
                NIOS_PKT_8x32_TX_RETUNE_CMD_ARM, 0, false);
    dwell_cfg_success = false;
    CHECK_WRITE(NIOS_PKT_8x32_TARGET_DWELL_READOUT, 0,
                UINT32_C(0x12345678), false);
    dwell_cfg_success = true;
    CHECK_WRITE(NIOS_PKT_8x32_TARGET_DWELL_READOUT, 0,
                UINT32_C(0x12345678), true);
    CHECK_WRITE(NIOS_PKT_8x32_TARGET_DWELL_READOUT, 1,
                UINT32_C(0x12345678), false);
    if (dwell_cfg_calls != 2) {
        fputs("dwell config accepted reserved write address\n", stderr);
        return 1;
    }
    CHECK_WRITE(NIOS_PKT_8x32_TARGET_PRETRIG_READ, 0, 0x300, true);
    CHECK_READ(NIOS_PKT_8x32_TARGET_PRETRIG_READ, 2, true, 0x302);
    CHECK_WRITE(NIOS_PKT_8x32_TARGET_PRETRIG_READ, 1, 0x400, false);
    CHECK_WRITE(NIOS_PKT_8x32_TARGET_PRETRIG_READ, 0, 0x1000, false);
    CHECK_READ(NIOS_PKT_8x32_TARGET_PRETRIG_READ, 2, true, 0x302);
    CHECK_WRITE(NIOS_PKT_8x32_TARGET_TX_RETUNE_GATE,
                NIOS_PKT_8x32_TX_RETUNE_CMD_COMPLETE, 0, false);
    gate_command_success = true;
    CHECK_WRITE(NIOS_PKT_8x32_TARGET_TX_RETUNE_GATE,
                NIOS_PKT_8x32_TX_RETUNE_CMD_COMPLETE, 0, true);
    CHECK_WRITE(NIOS_PKT_8x32_TARGET_TX_RETUNE_GATE,
                NIOS_PKT_8x32_TX_RETUNE_CMD_COMPLETE, 1, false);
    CHECK_WRITE(NIOS_PKT_8x32_TARGET_TX_RETUNE_GATE,
                NIOS_PKT_8x32_TX_RETUNE_CMD_ABORT, 1, false);
    gate_command_success = false;
    CHECK_WRITE(NIOS_PKT_8x32_TARGET_TX_RETUNE_GATE,
                NIOS_PKT_8x32_TX_RETUNE_CMD_FIFO_FLUSH, 0, false);
    gate_command_success = true;
    CHECK_WRITE(NIOS_PKT_8x32_TARGET_TX_RETUNE_GATE,
                NIOS_PKT_8x32_TX_RETUNE_CMD_FIFO_FLUSH, 0, true);
    CHECK_WRITE(NIOS_PKT_8x32_TARGET_TX_RETUNE_GATE,
                NIOS_PKT_8x32_TX_RETUNE_CMD_FIFO_RELEASE, 0, true);
    CHECK_WRITE(NIOS_PKT_8x32_TARGET_TX_RETUNE_GATE,
                NIOS_PKT_8x32_TX_RETUNE_CMD_FIFO_RELEASE, 1, false);
    if (gate_fifo_flush_calls != 2 || gate_fifo_release_calls != 1) {
        fputs("TX FIFO fault barrier command dispatch mismatch\n", stderr);
        return 1;
    }
    CHECK_WRITE(NIOS_PKT_8x32_TARGET_RX_EPOCH_SETTLE, 0, 0, false);

    /* RX spatial FIR: address bounds, mirrors follow only acknowledged
     * writes and commits, IDENTITY takes data 0 only. */
    CHECK_READ(NIOS_PKT_8x32_TARGET_RX_SPATIAL_FIR,
               NIOS_PKT_8x32_SFIR_ADDR_STATUS, true, UINT32_C(0x1003a5e3));
    CHECK_READ(NIOS_PKT_8x32_TARGET_RX_SPATIAL_FIR, 19, true, 0);
    CHECK_READ(NIOS_PKT_8x32_TARGET_RX_SPATIAL_FIR, 20, false, 0);
    CHECK_READ(NIOS_PKT_8x32_TARGET_RX_SPATIAL_FIR, 0x33, true, 0);
    CHECK_READ(NIOS_PKT_8x32_TARGET_RX_SPATIAL_FIR, 0x34, false, 0);
    CHECK_READ(NIOS_PKT_8x32_TARGET_RX_SPATIAL_FIR, 0x81, false, 0);
    sfir_success = true;
    CHECK_WRITE(NIOS_PKT_8x32_TARGET_RX_SPATIAL_FIR, 3,
                UINT32_C(0x12344000), true);
    CHECK_READ(NIOS_PKT_8x32_TARGET_RX_SPATIAL_FIR, 0x23, true,
               UINT32_C(0x12344000));
    CHECK_READ(NIOS_PKT_8x32_TARGET_RX_SPATIAL_FIR, 3, true, 0);
    CHECK_WRITE(NIOS_PKT_8x32_TARGET_RX_SPATIAL_FIR,
                NIOS_PKT_8x32_SFIR_CMD_COMMIT, 0x5a, true);
    CHECK_READ(NIOS_PKT_8x32_TARGET_RX_SPATIAL_FIR, 3, true,
               UINT32_C(0x12344000));
    CHECK_WRITE(NIOS_PKT_8x32_TARGET_RX_SPATIAL_FIR,
                NIOS_PKT_8x32_SFIR_CMD_COMMIT, 0x100, false);
    CHECK_WRITE(NIOS_PKT_8x32_TARGET_RX_SPATIAL_FIR, 20, 1, false);
    CHECK_WRITE(NIOS_PKT_8x32_TARGET_RX_SPATIAL_FIR, 0x23, 1, false);
    CHECK_WRITE(NIOS_PKT_8x32_TARGET_RX_SPATIAL_FIR,
                NIOS_PKT_8x32_SFIR_CMD_IDENTITY, 0, true);
    CHECK_WRITE(NIOS_PKT_8x32_TARGET_RX_SPATIAL_FIR,
                NIOS_PKT_8x32_SFIR_CMD_IDENTITY, 1, false);
    sfir_success = false;
    CHECK_WRITE(NIOS_PKT_8x32_TARGET_RX_SPATIAL_FIR, 3, 7, false);
    CHECK_READ(NIOS_PKT_8x32_TARGET_RX_SPATIAL_FIR, 0x23, true,
               UINT32_C(0x12344000));
    CHECK_WRITE(NIOS_PKT_8x32_TARGET_RX_SPATIAL_FIR,
                NIOS_PKT_8x32_SFIR_CMD_COMMIT, 0x11, false);
    CHECK_READ(NIOS_PKT_8x32_TARGET_RX_SPATIAL_FIR, 3, true,
               UINT32_C(0x12344000));
    sfir_success = true;
    if (sfir_write_calls != 2 || sfir_commit_calls != 2 ||
        sfir_identity_calls != 1 || sfir_last_tag != 0x11) {
        fputs("RX spatial FIR command dispatch mismatch\n", stderr);
        return 1;
    }

    /* Legacy fastlock packets must validate the full fields before any
     * narrowing or direction selection. */
    fastlock_save_calls = 0;
    CHECK_WRITE(NIOS_PKT_8x32_TARGET_FASTLOCK, 0,
                UINT32_C(0x00010002), true);
    if (fastlock_save_calls != 1 || fastlock_save_last_tx ||
        fastlock_save_last_rffe != 1 || fastlock_save_last_nios != 2) {
        fputs("fastlock RX packet fields were not preserved\n", stderr);
        return 1;
    }
    CHECK_WRITE(NIOS_PKT_8x32_TARGET_FASTLOCK, 1,
                UINT32_C(0x0007000f), true);
    if (fastlock_save_calls != 2 || !fastlock_save_last_tx ||
        fastlock_save_last_rffe != 7 || fastlock_save_last_nios != 15) {
        fputs("fastlock TX packet fields were not preserved\n", stderr);
        return 1;
    }
    CHECK_WRITE(NIOS_PKT_8x32_TARGET_FASTLOCK, 2,
                UINT32_C(0x00010002), false);
    CHECK_WRITE(NIOS_PKT_8x32_TARGET_FASTLOCK, 0,
                UINT32_C(0x00010102), false);
    CHECK_WRITE(NIOS_PKT_8x32_TARGET_FASTLOCK, 0,
                UINT32_C(0x00080002), false);
    CHECK_WRITE(NIOS_PKT_8x32_TARGET_FASTLOCK, 0,
                UINT32_C(0x01010002), false);
    if (fastlock_save_calls != 2) {
        fputs("malformed fastlock packet reached the profile saver\n",
              stderr);
        return 1;
    }
    fastlock_save_success = false;
    CHECK_WRITE(NIOS_PKT_8x32_TARGET_FASTLOCK, 0,
                UINT32_C(0x00010002), false);
    if (fastlock_save_calls != 3) {
        fputs("fastlock saver failure was not propagated\n", stderr);
        return 1;
    }

    puts("xA4 NIOS 8x32 diagnostic packet address status: PASS");
    return 0;
}
