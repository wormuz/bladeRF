#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "devices.h"
#include "nios_pkt_32x32.h"
#include "pkt_32x32.h"

static bool wishbone_success;
static bool expansion_direction_success;
static uint32_t expansion_direction;

uint32_t expansion_port_read(void) { return 0; }
void expansion_port_write(uint32_t data) { (void)data; }
uint32_t expansion_port_get_direction(void) { return 0; }
bool expansion_port_set_direction(uint32_t data)
{
    if (expansion_direction_success) {
        expansion_direction = data;
    }
    return expansion_direction == data;
}
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
    uint8_t packed[NIOS_PKT_LEN];
    struct pkt_buf packet = {
        .req = { NIOS_PKT_32x32_MAGIC, NIOS_PKT_32x32_TARGET_WB_MSTR,
                 0, 0, 0x10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
        .resp = { 0 }, .ready = false,
    };
    uint8_t id = 0;
    uint32_t addr = 0, data = UINT32_MAX;
    bool write = true, success = true;

    nios_pkt_32x32_pack(packed, NIOS_PKT_32x32_TARGET_WB_MSTR,
                        false, 0x10, 0);
    if (memcmp(packed, packet.req, sizeof(packed)) != 0) {
        return 1;
    }
    wishbone_success = false;
    pkt_32x32(&packet);
    nios_pkt_32x32_resp_unpack(packet.resp, &id, &write, &addr, &data,
                               &success);
    return id == NIOS_PKT_32x32_TARGET_WB_MSTR && !write && !success &&
           addr == 0x10 && data == 0 ? 0 : 1;
}

static int check_write_failure(void)
{
    uint8_t packed[NIOS_PKT_LEN];
    struct pkt_buf packet = {
        .req = { NIOS_PKT_32x32_MAGIC, NIOS_PKT_32x32_TARGET_WB_MSTR,
                 NIOS_PKT_32x32_FLAG_WRITE, 0,
                 0x10, 0, 0, 0, 0x5a, 0, 0, 0, 0, 0, 0, 0 },
        .resp = { 0 }, .ready = false,
    };
    uint8_t id = 0;
    uint32_t addr = 0, data = 0;
    bool write = false, success = true;

    nios_pkt_32x32_pack(packed, NIOS_PKT_32x32_TARGET_WB_MSTR,
                        true, 0x10, 0x5a);
    if (memcmp(packed, packet.req, sizeof(packed)) != 0) {
        return 1;
    }
    wishbone_success = false;
    pkt_32x32(&packet);
    nios_pkt_32x32_resp_unpack(packet.resp, &id, &write, &addr, &data,
                               &success);
    return id == NIOS_PKT_32x32_TARGET_WB_MSTR && write && !success &&
           addr == 0x10 && data == 0x5a ? 0 : 1;
}

static int check_expansion_direction_failure(void)
{
    struct pkt_buf packet = { .resp = { 0 }, .ready = false };
    uint8_t id = 0;
    uint32_t addr = 0, data = 0;
    bool write = false, success = true;

    nios_pkt_32x32_pack(packet.req, NIOS_PKT_32x32_TARGET_EXP_DIR,
                        true, UINT32_MAX, UINT32_C(0x12345678));
    expansion_direction_success = false;
    pkt_32x32(&packet);
    nios_pkt_32x32_resp_unpack(packet.resp, &id, &write, &addr, &data,
                               &success);
    if (id != NIOS_PKT_32x32_TARGET_EXP_DIR || !write || success) {
        return 1;
    }

    expansion_direction_success = true;
    pkt_32x32(&packet);
    nios_pkt_32x32_resp_unpack(packet.resp, &id, &write, &addr, &data,
                               &success);
    return id == NIOS_PKT_32x32_TARGET_EXP_DIR && write && success ? 0 : 1;
}

int main(void)
{
    if (check_read_failure() || check_write_failure() ||
        check_expansion_direction_failure()) {
        fputs("NIOS operation failure was reported as packet success\n", stderr);
        return 1;
    }
    puts("NIOS Wishbone packet failure status: PASS");
    return 0;
}
