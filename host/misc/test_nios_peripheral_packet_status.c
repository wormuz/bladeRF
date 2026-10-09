#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "devices.h"
#include "pkt_8x16.h"
#include "pkt_8x32.h"
#include "pkt_handler.h"
#include "nios_pkt_8x16.h"
#include "nios_pkt_8x32.h"

static bool peripheral_success;

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

uint16_t iqbal_get_gain(bladerf_module module)
{
    (void)module;
    return 0;
}

void iqbal_set_gain(bladerf_module module, uint16_t value)
{
    (void)module;
    (void)value;
}

uint16_t iqbal_get_phase(bladerf_module module)
{
    (void)module;
    return 0;
}

void iqbal_set_phase(bladerf_module module, uint16_t value)
{
    (void)module;
    (void)value;
}

void agc_dc_corr_write(uint16_t addr, uint16_t value)
{
    (void)addr;
    (void)value;
}

uint32_t control_reg_read(void) { return 0; }
void control_reg_write(uint32_t value) { (void)value; }
uint32_t rffe_csr_read(void) { return 0; }
void rffe_csr_write(uint32_t value) { (void)value; }
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

int main(void)
{
    if (check_8x16_write_failure() || check_8x16_read_failure() ||
        check_8x32_write_failure()) {
        fputs("peripheral SPI failure was reported as packet success\n", stderr);
        return 1;
    }
    puts("NIOS peripheral SPI packet failure status: PASS");
    return 0;
}
