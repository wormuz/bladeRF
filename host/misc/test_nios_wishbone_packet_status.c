#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "devices.h"
#include "nios_pkt_32x32.h"
#include "pkt_32x32.h"

static struct pkt_buf make_32x32_packet(uint8_t target, bool write,
                                        uint32_t addr, uint32_t data)
{
    uint8_t request[NIOS_PKT_LEN] = { 0 };
    nios_pkt_32x32_pack(request, target, write, addr, data);
    return (struct pkt_buf) {
        .req = { request[0], request[1], request[2], request[3],
                 request[4], request[5], request[6], request[7],
                 request[8], request[9], request[10], request[11],
                 request[12], request[13], request[14], request[15] },
        .resp = { 0 }, .ready = false,
    };
}

static bool wishbone_success;
static bool expansion_direction_success;
static uint32_t expansion_direction;
static bool expansion_write_success;
static uint32_t expansion_output;

static int check_32bit_packet_round_trip(void)
{
    uint8_t packed[NIOS_PKT_LEN];
    uint8_t id = 0;
    uint32_t addr = 0, data = 0;
    bool write = false;

    nios_pkt_32x32_pack(packed, NIOS_PKT_32x32_TARGET_WB_MSTR, true,
                        UINT32_C(0x87654321), UINT32_C(0xfedcba98));
    nios_pkt_32x32_unpack(packed, &id, &write, &addr, &data);
    return id == NIOS_PKT_32x32_TARGET_WB_MSTR && write &&
           addr == UINT32_C(0x87654321) &&
           data == UINT32_C(0xfedcba98) ? 0 : 1;
}

uint32_t expansion_port_read(void) { return expansion_output; }
bool expansion_port_write(uint32_t data)
{
    if (expansion_write_success) {
        expansion_output = data;
    }
    return (expansion_output & expansion_direction) ==
           (data & expansion_direction);
}
uint32_t expansion_port_get_direction(void) { return expansion_direction; }
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
    struct pkt_buf packet = make_32x32_packet(
        NIOS_PKT_32x32_TARGET_EXP_DIR, true, UINT32_MAX,
        UINT32_C(0x12345678));
    uint8_t id = 0;
    uint32_t addr = 0, data = 0;
    bool write = false, success = true;

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

static int check_expansion_data_failure(void)
{
    struct pkt_buf packet = make_32x32_packet(
        NIOS_PKT_32x32_TARGET_EXP, true, UINT32_MAX,
        UINT32_C(0x87654321));
    uint8_t id = 0;
    uint32_t addr = 0, data = 0;
    bool write = false, success = true;

    expansion_direction = UINT32_MAX;
    expansion_output = 0;
    expansion_write_success = false;
    pkt_32x32(&packet);
    nios_pkt_32x32_resp_unpack(packet.resp, &id, &write, &addr, &data,
                               &success);
    if (id != NIOS_PKT_32x32_TARGET_EXP || !write || success) {
        return 1;
    }

    expansion_write_success = true;
    pkt_32x32(&packet);
    nios_pkt_32x32_resp_unpack(packet.resp, &id, &write, &addr, &data,
                               &success);
    return id == NIOS_PKT_32x32_TARGET_EXP && write && success ? 0 : 1;
}

int main(void)
{
    if (check_32bit_packet_round_trip() || check_read_failure() ||
        check_write_failure() ||
        check_expansion_direction_failure() ||
        check_expansion_data_failure()) {
        fputs("NIOS operation failure was reported as packet success\n", stderr);
        return 1;
    }
    puts("NIOS Wishbone packet failure status: PASS");
    return 0;
}
