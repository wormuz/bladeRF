#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "devices.h"
#include "pkt_8x16.h"
#include "nios_pkt_8x16.h"

static bool ina_ok;
static bool ad56_ok;
static unsigned int session_calls;
static uint32_t session_seen;
static bool session_before_op;

void i2c_session_begin(uint32_t session_generation)
{
    ++session_calls;
    session_seen = session_generation;
    session_before_op = true;
}

bool ina219_read(uint8_t addr, uint16_t *value)
{
    (void)addr;
    if (!session_before_op) return false;
    *value = ina_ok ? 0x1234 : 0;
    return ina_ok;
}

bool ina219_write(uint8_t addr, uint16_t value)
{
    (void)addr;
    (void)value;
    return session_before_op && ina_ok;
}

bool vctcxo_trim_dac_write(uint8_t cmd, uint16_t val)
{ (void)cmd; (void)val; return true; }
bool vctcxo_trim_dac_read(uint8_t cmd, uint16_t *val)
{ (void)cmd; *val = 0; return true; }
bool ad56x1_vctcxo_trim_dac_write(uint16_t val) { (void)val; return ad56_ok; }
void ad56x1_vctcxo_trim_dac_read(uint16_t *val) { *val = 0; }
bool iqbal_get_gain(bladerf_module m, uint16_t *v)
{
    (void)m;
    if (v == NULL) return false;
    *v = 0;
    return true;
}
bool iqbal_set_gain(bladerf_module m, uint16_t v)
{ (void)m; (void)v; return true; }
bool iqbal_get_phase(bladerf_module m, uint16_t *v)
{
    (void)m;
    if (v == NULL) return false;
    *v = 0;
    return true;
}
bool iqbal_set_phase(bladerf_module m, uint16_t v)
{ (void)m; (void)v; return true; }
bool agc_dc_corr_write(uint16_t a, uint16_t v) { (void)a; (void)v; return false; }
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
    /* The controller re-initialisation runs before every INA219 access,
     * with the request's own session. */
    packet.session_generation = 0x5e55u + session_calls;
    session_before_op = false;
    session_seen = 0;
    pkt_8x16(&packet);
    nios_pkt_8x16_resp_unpack(packet.resp, &target, &response_write, &addr,
                              &data, &response_success);
    return session_seen == packet.session_generation &&
           target == NIOS_PKT_8x16_TARGET_INA219 &&
           response_write == write && response_success == success;
}

static bool run_ad56x1(bool write, uint8_t addr, bool success)
{
    struct pkt_buf packet = {
        .req = { NIOS_PKT_8x16_MAGIC, NIOS_PKT_8x16_TARGET_AD56X1_DAC,
                 write ? NIOS_PKT_8x16_FLAG_WRITE : 0, 0, addr,
                 write ? 0x12 : 0, write ? 0x34 : 0 },
        .resp = { 0 }, .ready = false,
    };
    uint8_t target, response_addr;
    uint16_t data;
    bool response_write, response_success;

    const unsigned int calls_before = session_calls;

    ad56_ok = success;
    pkt_8x16(&packet);
    nios_pkt_8x16_resp_unpack(packet.resp, &target, &response_write,
                              &response_addr, &data, &response_success);
    return session_calls == calls_before &&
           target == NIOS_PKT_8x16_TARGET_AD56X1_DAC &&
           response_addr == addr && response_write == write &&
           response_success == success;
}

int main(void)
{
    if (!run(false, false) || !run(true, false) ||
        !run(false, true) || !run(true, true) ||
        !run_ad56x1(true, 0, false) || !run_ad56x1(true, 0, true) ||
        !run_ad56x1(false, 1, false) || !run_ad56x1(true, 1, false)) {
        fputs("INA219 failure/success packet status mismatch\n", stderr);
        return 1;
    }
    puts("NIOS INA219 packet failure/success status: PASS");
    return 0;
}
