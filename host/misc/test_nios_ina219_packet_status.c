#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "devices.h"
#include "pkt_8x16.h"
#include "nios_pkt_8x16.h"

static bool ina_ok;

bool ina219_read(uint8_t addr, uint16_t *value)
{
    (void)addr;
    *value = ina_ok ? 0x1234 : 0;
    return ina_ok;
}

bool ina219_write(uint8_t addr, uint16_t value)
{
    (void)addr;
    (void)value;
    return ina_ok;
}

bool vctcxo_trim_dac_write(uint8_t cmd, uint16_t val)
{ (void)cmd; (void)val; return true; }
bool vctcxo_trim_dac_read(uint8_t cmd, uint16_t *val)
{ (void)cmd; *val = 0; return true; }
bool ad56x1_vctcxo_trim_dac_write(uint16_t val) { (void)val; return true; }
void ad56x1_vctcxo_trim_dac_read(uint16_t *val) { *val = 0; }
uint16_t iqbal_get_gain(bladerf_module m) { (void)m; return 0; }
void iqbal_set_gain(bladerf_module m, uint16_t v) { (void)m; (void)v; }
uint16_t iqbal_get_phase(bladerf_module m) { (void)m; return 0; }
void iqbal_set_phase(bladerf_module m, uint16_t v) { (void)m; (void)v; }
void agc_dc_corr_write(uint16_t a, uint16_t v) { (void)a; (void)v; }
uint32_t rffe_csr_read(void) { return 0; }
void rffe_csr_write(uint32_t v) { (void)v; }

static bool run(bool write, bool success)
{
    struct pkt_buf packet = {
        .req = { NIOS_PKT_8x16_MAGIC, NIOS_PKT_8x16_TARGET_INA219,
                 write ? NIOS_PKT_8x16_FLAG_WRITE : 0, 0, 0x02,
                 write ? 0x12 : 0, write ? 0x34 : 0 },
        .resp = { 0 }, .ready = false,
    };
    uint8_t target, addr;
    uint16_t data;
    bool response_write, response_success;

    ina_ok = success;
    pkt_8x16(&packet);
    nios_pkt_8x16_resp_unpack(packet.resp, &target, &response_write, &addr,
                              &data, &response_success);
    return target == NIOS_PKT_8x16_TARGET_INA219 &&
           response_write == write && response_success == success;
}

int main(void)
{
    if (!run(false, false) || !run(true, false) ||
        !run(false, true) || !run(true, true)) {
        fputs("INA219 failure/success packet status mismatch\n", stderr);
        return 1;
    }
    puts("NIOS INA219 packet failure/success status: PASS");
    return 0;
}
