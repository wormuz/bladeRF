#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "devices.h"
#include "pkt_8x16.h"
#include "pkt_8x8.h"
#include "pkt_8x32.h"
#include "pkt_handler.h"
#include "nios_pkt_8x16.h"
#include "nios_pkt_8x8.h"
#include "nios_pkt_8x32.h"
#include "nios_pkt_8x32_diag.h"

static bool peripheral_success;
static bool lms_success;
static bool tamer_mode_success;
static bool agc_dc_success;
static bool iqcorr_success;
static bladerf_vctcxo_tamer_mode tamer_mode_readback =
    BLADERF_VCTCXO_TAMER_DISABLED;

bool si5338_read(uint8_t addr, uint8_t *value)
{
    (void)addr;
    if (peripheral_success) {
        *value = 0x5a;
    }
    return peripheral_success;
}

bool si5338_write(uint8_t addr, uint8_t value)
{
    (void)addr;
    (void)value;
    return peripheral_success;
}

bool ina219_read(uint8_t addr, uint16_t *value)
{
    (void)addr;
    if (peripheral_success) {
        *value = 0x1234;
    }
    return peripheral_success;
}

bool ina219_write(uint8_t addr, uint16_t value)
{
    (void)addr;
    (void)value;
    return peripheral_success;
}

bool lms6_read(uint8_t addr, uint8_t *value)
{
    (void)addr;
    if (lms_success && value != NULL) {
        *value = 0x5a;
        return true;
    }
    return false;
}
bool lms6_write(uint8_t addr, uint8_t value)
{
    (void)addr;
    (void)value;
    return lms_success;
}
bladerf_vctcxo_tamer_mode vctcxo_tamer_get_tune_mode(void)
{ return tamer_mode_readback; }
bool vctcxo_tamer_set_tune_mode(bladerf_vctcxo_tamer_mode mode)
{
    return tamer_mode_success &&
           (mode == BLADERF_VCTCXO_TAMER_DISABLED ||
            mode == BLADERF_VCTCXO_TAMER_1_PPS ||
            mode == BLADERF_VCTCXO_TAMER_10_MHZ);
}
uint8_t tx_trigger_ctl_read(void) { return 0; }
void tx_trigger_ctl_write(uint8_t value) { (void)value; }
uint8_t rx_trigger_ctl_read(void) { return 0; }
void rx_trigger_ctl_write(uint8_t value) { (void)value; }

bool vctcxo_trim_dac_write(uint8_t cmd, uint16_t value)
{
    (void)cmd;
    (void)value;
    return peripheral_success;
}

bool vctcxo_trim_dac_read(uint8_t cmd, uint16_t *value)
{
    (void)cmd;
    if (peripheral_success) {
        *value = 0x1234;
    }
    return peripheral_success;
}

bool iqbal_get_gain(bladerf_module module, uint16_t *value)
{
    (void)module;
    if (!iqcorr_success || value == NULL) return false;
    *value = 0;
    return true;
}

bool iqbal_set_gain(bladerf_module module, uint16_t value)
{
    (void)module;
    (void)value;
    return iqcorr_success;
}

bool iqbal_get_phase(bladerf_module module, uint16_t *value)
{
    (void)module;
    if (!iqcorr_success || value == NULL) return false;
    *value = 0;
    return iqcorr_success;
}

bool iqbal_set_phase(bladerf_module module, uint16_t value)
{
    (void)module;
    (void)value;
    return iqcorr_success;
}

bool agc_dc_corr_write(uint16_t addr, uint16_t value)
{
    (void)value;
    return agc_dc_success && addr <= NIOS_PKT_8x16_ADDR_AGC_DC_I_MIN;
}

uint32_t control_reg_read(void) { return 0; }
bool control_reg_write(uint32_t value) { (void)value; return true; }
uint32_t rffe_csr_read(void) { return 0; }
void rffe_csr_write(uint32_t value) { (void)value; }
bool rffe_csr_read_checked(uint32_t *value)
{
    if (value == NULL) {
        return false;
    }
    *value = rffe_csr_read();
    return true;
}
bool rffe_csr_write_checked(uint32_t value, uint32_t verify_mask)
{
    (void)value;
    (void)verify_mask;
    return true;
}
bool adf4351_write(uint32_t value)
{
    (void)value;
    return peripheral_success;
}

static int check_8x16_write_failure(void)
{
    struct pkt_buf packet = {
        .req = { NIOS_PKT_8x16_MAGIC, NIOS_PKT_8x16_TARGET_VCTCXO_DAC,
                 NIOS_PKT_8x16_FLAG_WRITE, 0, 0x08, 0x12, 0x80 },
        .resp = { 0 }, .ready = false,
    };
    uint8_t target = 0, addr = 0;
    uint16_t data = 0;
    bool write = false, success = true;

    peripheral_success = false;
    pkt_8x16(&packet);
    nios_pkt_8x16_resp_unpack(packet.resp, &target, &write, &addr, &data,
                              &success);
    return target == NIOS_PKT_8x16_TARGET_VCTCXO_DAC && write &&
           addr == 0x08 && !success ? 0 : 1;
}

static int check_8x16_read_failure(void)
{
    struct pkt_buf packet = {
        .req = { NIOS_PKT_8x16_MAGIC, NIOS_PKT_8x16_TARGET_VCTCXO_DAC,
                 0, 0, 0x98, 0, 0 },
        .resp = { 0 }, .ready = false,
    };
    uint8_t target = 0, addr = 0;
    uint16_t data = 0;
    bool write = true, success = true;

    peripheral_success = false;
    pkt_8x16(&packet);
    nios_pkt_8x16_resp_unpack(packet.resp, &target, &write, &addr, &data,
                              &success);
    return target == NIOS_PKT_8x16_TARGET_VCTCXO_DAC && !write &&
           addr == 0x98 && !success ? 0 : 1;
}

static int check_8x32_write_failure(void)
{
    struct pkt_buf packet = {
        .req = { NIOS_PKT_8x32_MAGIC, NIOS_PKT_8x32_TARGET_ADF4351,
                 NIOS_PKT_8x32_FLAG_WRITE, 0, 0, 0x05, 0, 0x58, 0 },
        .resp = { 0 }, .ready = false,
    };
    uint8_t target = 0, addr = 0;
    uint32_t data = 0;
    bool write = false, success = true;

    peripheral_success = false;
    pkt_8x32(&packet);
    nios_pkt_8x32_resp_unpack(packet.resp, &target, &write, &addr, &data,
                              &success);
    return target == NIOS_PKT_8x32_TARGET_ADF4351 && write && !success ? 0 : 1;
}

static int check_8x8_si5338_failure(void)
{
    struct pkt_buf packet = {
        .req = { NIOS_PKT_8x8_MAGIC, NIOS_PKT_8x8_TARGET_SI5338,
                 NIOS_PKT_8x8_FLAG_WRITE, 0, 0x05, 0xab },
        .resp = { 0 }, .ready = false,
    };
    uint8_t target = 0, addr = 0, data = 0;
    bool write = false, success = true;

    peripheral_success = false;
    pkt_8x8(&packet);
    nios_pkt_8x8_resp_unpack(packet.resp, &target, &write, &addr, &data,
                             &success);
    return target == NIOS_PKT_8x8_TARGET_SI5338 && write && !success ? 0 : 1;
}

static int check_8x8_lms_failure(void)
{
    struct pkt_buf packet = {
        .req = { NIOS_PKT_8x8_MAGIC, NIOS_PKT_8x8_TARGET_LMS6,
                 NIOS_PKT_8x8_FLAG_WRITE, 0, 0x05, 0xab },
        .resp = { 0 }, .ready = false,
    };
    uint8_t target = 0, addr = 0, data = 0;
    bool write = false, success = true;

    lms_success = false;
    pkt_8x8(&packet);
    nios_pkt_8x8_resp_unpack(packet.resp, &target, &write, &addr, &data,
                             &success);
    return target == NIOS_PKT_8x8_TARGET_LMS6 && write && !success ? 0 : 1;
}

static int check_8x8_lms_read_failure(void)
{
    struct pkt_buf packet = {
        .req = { NIOS_PKT_8x8_MAGIC, NIOS_PKT_8x8_TARGET_LMS6,
                 0, 0, 0x05, 0 },
        .resp = { 0 }, .ready = false,
    };
    uint8_t target = 0, addr = 0, data = 0;
    bool write = true, success = true;

    lms_success = false;
    pkt_8x8(&packet);
    nios_pkt_8x8_resp_unpack(packet.resp, &target, &write, &addr, &data,
                             &success);
    return target == NIOS_PKT_8x8_TARGET_LMS6 && !write && !success ? 0 : 1;
}

static int check_8x8_tamer_set_failure(void)
{
    struct pkt_buf packet = {
        .req = { NIOS_PKT_8x8_MAGIC, NIOS_PKT_8x8_TARGET_VCTCXO_TAMER,
                 NIOS_PKT_8x8_FLAG_WRITE, 0, 0xff,
                 BLADERF_VCTCXO_TAMER_1_PPS },
        .resp = { 0 }, .ready = false,
    };
    uint8_t target = 0, addr = 0, data = 0;
    bool write = false, success = true;

    tamer_mode_success = false;
    pkt_8x8(&packet);
    nios_pkt_8x8_resp_unpack(packet.resp, &target, &write, &addr, &data,
                             &success);
    return target == NIOS_PKT_8x8_TARGET_VCTCXO_TAMER && write &&
           addr == 0xff && !success ? 0 : 1;
}

static int check_8x8_tamer_invalid_mode(void)
{
    struct pkt_buf packet = {
        .req = { NIOS_PKT_8x8_MAGIC, NIOS_PKT_8x8_TARGET_VCTCXO_TAMER,
                 NIOS_PKT_8x8_FLAG_WRITE, 0, 0xff, 0xff },
        .resp = { 0 }, .ready = false,
    };
    uint8_t target = 0, addr = 0, data = 0;
    bool write = false, success = true;

    tamer_mode_success = true;
    pkt_8x8(&packet);
    nios_pkt_8x8_resp_unpack(packet.resp, &target, &write, &addr, &data,
                             &success);
    return target == NIOS_PKT_8x8_TARGET_VCTCXO_TAMER && write &&
           addr == 0xff && !success ? 0 : 1;
}

static int check_8x8_tamer_set_success(void)
{
    struct pkt_buf packet = {
        .req = { NIOS_PKT_8x8_MAGIC, NIOS_PKT_8x8_TARGET_VCTCXO_TAMER,
                 NIOS_PKT_8x8_FLAG_WRITE, 0, 0xff,
                 BLADERF_VCTCXO_TAMER_1_PPS },
        .resp = { 0 }, .ready = false,
    };
    uint8_t target = 0, addr = 0, data = 0;
    bool write = false, success = false;

    tamer_mode_success = true;
    pkt_8x8(&packet);
    nios_pkt_8x8_resp_unpack(packet.resp, &target, &write, &addr, &data,
                             &success);
    return target == NIOS_PKT_8x8_TARGET_VCTCXO_TAMER && write &&
           addr == 0xff && success ? 0 : 1;
}

static int check_8x8_tamer_invalid_readback(void)
{
    struct pkt_buf packet = {
        .req = { NIOS_PKT_8x8_MAGIC, NIOS_PKT_8x8_TARGET_VCTCXO_TAMER,
                 0, 0, 0xff, 0 },
        .resp = { 0 }, .ready = false,
    };
    uint8_t target = 0, addr = 0, data = 0xff;
    bool write = true, success = true;

    tamer_mode_readback = BLADERF_VCTCXO_TAMER_INVALID;
    pkt_8x8(&packet);
    nios_pkt_8x8_resp_unpack(packet.resp, &target, &write, &addr, &data,
                             &success);
    tamer_mode_readback = BLADERF_VCTCXO_TAMER_DISABLED;
    return target == NIOS_PKT_8x8_TARGET_VCTCXO_TAMER && !write &&
           addr == 0xff && !success ? 0 : 1;
}

static int check_8x8_tamer_invalid_subaddress(void)
{
    struct pkt_buf read_packet = {
        .req = { NIOS_PKT_8x8_MAGIC, NIOS_PKT_8x8_TARGET_VCTCXO_TAMER,
                 0, 0, 0x01, 0 },
        .resp = { 0 }, .ready = false,
    };
    struct pkt_buf write_packet = {
        .req = { NIOS_PKT_8x8_MAGIC, NIOS_PKT_8x8_TARGET_VCTCXO_TAMER,
                 NIOS_PKT_8x8_FLAG_WRITE, 0, 0x01,
                 BLADERF_VCTCXO_TAMER_1_PPS },
        .resp = { 0 }, .ready = false,
    };
    uint8_t target = 0, addr = 0, data = 0;
    bool write = true, success = true;

    pkt_8x8(&read_packet);
    nios_pkt_8x8_resp_unpack(read_packet.resp, &target, &write, &addr, &data,
                             &success);
    if (target != NIOS_PKT_8x8_TARGET_VCTCXO_TAMER || write ||
        addr != 0x01 || success) {
        return 1;
    }

    write = false;
    success = true;
    pkt_8x8(&write_packet);
    nios_pkt_8x8_resp_unpack(write_packet.resp, &target, &write, &addr, &data,
                             &success);
    return target == NIOS_PKT_8x8_TARGET_VCTCXO_TAMER && write &&
           addr == 0x01 && !success ? 0 : 1;
}

static int check_8x8_trigger_subaddresses(void)
{
    static const uint8_t targets[] = {
        NIOS_PKT_8x8_TX_TRIGGER_CTL, NIOS_PKT_8x8_RX_TRIGGER_CTL,
    };
    struct pkt_buf packet = { .resp = { 0 }, .ready = false };
    uint8_t target, addr, data;
    bool write, success;
    unsigned int i;

    for (i = 0; i < sizeof(targets) / sizeof(targets[0]); ++i) {
        nios_pkt_8x8_pack(packet.req, targets[i], false, 1, 0);
        pkt_8x8(&packet);
        nios_pkt_8x8_resp_unpack(packet.resp, &target, &write, &addr, &data,
                                 &success);
        if (target != targets[i] || write || addr != 1 || success) {
            return 1;
        }

        nios_pkt_8x8_pack(packet.req, targets[i], true, 1, 0x5a);
        pkt_8x8(&packet);
        nios_pkt_8x8_resp_unpack(packet.resp, &target, &write, &addr, &data,
                                 &success);
        if (target != targets[i] || !write || addr != 1 || success) {
            return 1;
        }
    }

    return 0;
}

static int check_8x16_agc_write_status(void)
{
    struct pkt_buf packet = { .resp = { 0 }, .ready = false };
    uint8_t target, addr;
    uint16_t data;
    bool write, success;

    nios_pkt_8x16_pack(packet.req, NIOS_PKT_8x16_TARGET_AGC_CORR,
                       true, NIOS_PKT_8x16_ADDR_AGC_DC_Q_MAX, 0x1234);
    agc_dc_success = false;
    pkt_8x16(&packet);
    nios_pkt_8x16_resp_unpack(packet.resp, &target, &write, &addr, &data,
                              &success);
    if (target != NIOS_PKT_8x16_TARGET_AGC_CORR || !write || success) {
        return 1;
    }

    nios_pkt_8x16_pack(packet.req, NIOS_PKT_8x16_TARGET_AGC_CORR,
                       true, NIOS_PKT_8x16_ADDR_AGC_DC_Q_MAX, 0x1234);
    agc_dc_success = true;
    pkt_8x16(&packet);
    nios_pkt_8x16_resp_unpack(packet.resp, &target, &write, &addr, &data,
                              &success);
    if (target != NIOS_PKT_8x16_TARGET_AGC_CORR || !write || !success) {
        return 1;
    }

    nios_pkt_8x16_pack(packet.req, NIOS_PKT_8x16_TARGET_AGC_CORR,
                       true, 0xff, 0x1234);
    pkt_8x16(&packet);
    nios_pkt_8x16_resp_unpack(packet.resp, &target, &write, &addr, &data,
                              &success);
    return target == NIOS_PKT_8x16_TARGET_AGC_CORR && write && !success ? 0 : 1;
}

static int check_8x16_iq_correction_status(void)
{
    struct pkt_buf packet = { .resp = { 0 }, .ready = false };
    uint8_t target, addr;
    uint16_t data;
    bool write, success;

    nios_pkt_8x16_pack(packet.req, NIOS_PKT_8x16_TARGET_IQ_CORR,
                       false, NIOS_PKT_8x16_ADDR_IQ_CORR_RX_GAIN, 0);
    pkt_8x16(&packet);
    nios_pkt_8x16_resp_unpack(packet.resp, &target, &write, &addr, &data,
                              &success);
    if (target != NIOS_PKT_8x16_TARGET_IQ_CORR || write || success) {
        return 1;
    }

    nios_pkt_8x16_pack(packet.req, NIOS_PKT_8x16_TARGET_IQ_CORR,
                       true, NIOS_PKT_8x16_ADDR_IQ_CORR_RX_PHASE, 0x1234);
    pkt_8x16(&packet);
    nios_pkt_8x16_resp_unpack(packet.resp, &target, &write, &addr, &data,
                              &success);
    if (target != NIOS_PKT_8x16_TARGET_IQ_CORR || !write || success) {
        return 1;
    }

    iqcorr_success = true;
    pkt_8x16(&packet);
    nios_pkt_8x16_resp_unpack(packet.resp, &target, &write, &addr, &data,
                              &success);
    return target == NIOS_PKT_8x16_TARGET_IQ_CORR && write && success ? 0 : 1;
}

static int check_8x32_diagnostic_address_bounds(void)
{
    static const struct {
        uint8_t target;
        uint8_t valid_addr;
        uint8_t invalid_addr;
    } bounds[] = {
        { NIOS_PKT_8x32_TARGET_RETUNE2_FAILURES, 1, 2 },
        { NIOS_PKT_8x32_TARGET_RETUNE2_SUCCESSES, 1, 2 },
        { NIOS_PKT_8x32_TARGET_RETUNE2_COMPLETION, 5, 6 },
        { NIOS_PKT_8x32_TARGET_LOSS_COUNTERS, 3, 4 },
        { NIOS_PKT_8x32_TARGET_DWELL_READOUT, 15, 16 },
    };
    unsigned int i;

    for (i = 0; i < sizeof(bounds) / sizeof(bounds[0]); ++i) {
        if (!nios_pkt_8x32_diagnostic_read_addr_valid(
                bounds[i].target, bounds[i].valid_addr) ||
            nios_pkt_8x32_diagnostic_read_addr_valid(
                bounds[i].target, bounds[i].invalid_addr)) {
            return 1;
        }
    }
    return 0;
}

int main(void)
{
    if (check_8x16_write_failure() || check_8x16_read_failure() ||
        check_8x32_write_failure() || check_8x8_si5338_failure() ||
        check_8x8_lms_failure() || check_8x8_lms_read_failure() ||
        check_8x8_tamer_set_failure() || check_8x8_tamer_invalid_mode() ||
        check_8x8_tamer_set_success() || check_8x8_tamer_invalid_readback() ||
        check_8x8_tamer_invalid_subaddress() ||
        check_8x8_trigger_subaddresses() || check_8x16_agc_write_status() ||
        check_8x16_iq_correction_status() ||
        check_8x32_diagnostic_address_bounds()) {
        fputs("peripheral failure was reported as packet success\n", stderr);
        return 1;
    }
    puts("NIOS peripheral packet failure status: PASS");
    return 0;
}
