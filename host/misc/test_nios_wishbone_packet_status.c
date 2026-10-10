#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "devices.h"
#include "nios_pkt_32x32.h"
#include "pkt_32x32.h"

static bool wishbone_success;

uint32_t expansion_port_read(void) { return 0; }
void expansion_port_write(uint32_t data) { (void)data; }
uint32_t expansion_port_get_direction(void) { return 0; }
void expansion_port_set_direction(uint32_t data) { (void)data; }
bool adi_axi_read(uint32_t addr, uint32_t *data)
{
    (void)addr;
    if (data != NULL) {
        *data = 0;
    }
    return true;
}
bool adi_axi_write(uint32_t addr, uint32_t data)
{
    (void)addr;
    (void)data;
    return true;
}

bool wishbone_master_read(uint32_t addr, uint32_t *data)
{
    (void)addr;
    if (!wishbone_success || data == NULL) {
        return false;
    }
    *data = UINT32_C(0x12345678);
    return true;
}

bool wishbone_master_write(uint32_t addr, uint32_t data)
{
    (void)addr;
    (void)data;
    return wishbone_success;
}

static int check_read_failure(void)
{
    struct pkt_buf packet = {
        .resp = { 0 }, .ready = false,
    };
    uint8_t id = 0;
    uint32_t addr = 0, data = UINT32_MAX;
    bool write = true, success = true;

    nios_pkt_32x32_pack(packet.req, NIOS_PKT_32x32_TARGET_WB_MSTR,
                        false, 0x10, 0);
    wishbone_success = false;
    pkt_32x32(&packet);
    nios_pkt_32x32_resp_unpack(packet.resp, &id, &write, &addr, &data,
                               &success);
    return id == NIOS_PKT_32x32_TARGET_WB_MSTR && !write && !success &&
           addr == 0x10 && data == 0 ? 0 : 1;
}

static int check_write_failure(void)
{
    struct pkt_buf packet = {
        .resp = { 0 }, .ready = false,
    };
    uint8_t id = 0;
    uint32_t addr = 0, data = 0;
    bool write = false, success = true;

    nios_pkt_32x32_pack(packet.req, NIOS_PKT_32x32_TARGET_WB_MSTR,
                        true, 0x10, 0x5a);
    wishbone_success = false;
    pkt_32x32(&packet);
    nios_pkt_32x32_resp_unpack(packet.resp, &id, &write, &addr, &data,
                               &success);
    return id == NIOS_PKT_32x32_TARGET_WB_MSTR && write && !success &&
           addr == 0x10 && data == 0x5a ? 0 : 1;
}

int main(void)
{
    if (check_read_failure() || check_write_failure()) {
        fputs("Wishbone failure was reported as NIOS packet success\n", stderr);
        return 1;
    }
    puts("NIOS Wishbone packet failure status: PASS");
    return 0;
}
