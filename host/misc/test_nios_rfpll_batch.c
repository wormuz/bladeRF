#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pkt_handler.h"
#include "pkt_16x64.h"
#include "devices.h"

static struct pkt_buf make_16x64_packet(uint8_t target, bool write,
                                        uint16_t addr, uint64_t data)
{
    uint8_t request[NIOS_PKT_LEN] = { 0 };
    nios_pkt_16x64_pack(request, target, write, addr, data);
    return (struct pkt_buf) {
        .req = { request[0], request[1], request[2], request[3],
                 request[4], request[5], request[6], request[7],
                 request[8], request[9], request[10], request[11],
                 request[12], request[13], request[14], request[15] },
        .resp = { 0 }, .ready = false,
    };
}

enum batch_mode {
    BATCH_SUCCESS,
    BATCH_FAILURE,
    BATCH_PARTIAL,
};

static enum batch_mode mode;
static unsigned int calls;
static unsigned int write_calls;
static bool write_success = true;
static unsigned int update_calls;
static bool update_success = true;
static unsigned int read_calls;
static bool read_success = true;
static unsigned int gain_row_calls;
static unsigned int epoch_snapshot_calls;

bool adi_spi_gain_table_row(uint16_t row, uint8_t data1, uint8_t data2,
                            uint8_t data3, uint8_t config,
                            uint32_t delay_us)
{
    gain_row_calls++;
    return row == 7 && data1 == 0x11 && data2 == 0x22 &&
           data3 == 0x33 && config == 0x44 && delay_us == 2;
}

static int run_gain_row_case(uint64_t payload, bool expected_success,
                             unsigned int expected_calls)
{
    struct pkt_buf packet = {
        .req = { 0x45, NIOS_PKT_16x64_TARGET_GAIN_TABLE_ROW,
                 NIOS_PKT_16x64_FLAG_WRITE, 0, 7, 0,
                 (uint8_t)payload, (uint8_t)(payload >> 8),
                 (uint8_t)(payload >> 16), (uint8_t)(payload >> 24),
                 (uint8_t)(payload >> 32), (uint8_t)(payload >> 40),
                 (uint8_t)(payload >> 48), (uint8_t)(payload >> 56),
                 0, 0 },
        .ready = false,
    };
    uint8_t target = 0;
    uint16_t addr = 0;
    uint64_t response = 0;
    bool write = false, success = false;

    pkt_16x64(&packet);
    nios_pkt_16x64_resp_unpack(packet.resp, &target, &write, &addr,
                               &response, &success);
    return target == NIOS_PKT_16x64_TARGET_GAIN_TABLE_ROW && write &&
           addr == 7 && success == expected_success &&
           gain_row_calls == expected_calls ? 0 : 1;
}

bool adi_spi_read_checked(uint16_t addr, uint64_t *value)
{
    read_calls++;
    if (addr != 0x247 || value == NULL || !read_success) {
        return false;
    }
    *value = UINT64_C(0x0200000000000000);
    return true;
}

bool adi_spi_write(uint16_t addr, uint64_t data)
{
    write_calls++;
    return addr == 0x247 && data == UINT64_C(0x123456789abcdef0) &&
           write_success;
}

static int run_ad9361_write(bool requested_success, bool expected_success)
{
    struct pkt_buf packet = {
        .req = { 0x45, NIOS_PKT_16x64_TARGET_AD9361,
                 NIOS_PKT_16x64_FLAG_WRITE, 0, 0x47, 0x02,
                 0xf0, 0xde, 0xbc, 0x9a, 0x78, 0x56, 0x34, 0x12,
                 0, 0 },
        .ready = false,
    };
    uint8_t target = 0;
    uint16_t addr = 0;
    uint64_t response = 0;
    bool write = false, success = false;
    const unsigned int expected_calls = write_calls + 1;

    write_success = requested_success;
    pkt_16x64(&packet);
    nios_pkt_16x64_resp_unpack(packet.resp, &target, &write, &addr,
                               &response, &success);
    return target == NIOS_PKT_16x64_TARGET_AD9361 && write && addr == 0x247 &&
           success == expected_success && write_calls == expected_calls ? 0 : 1;
}

static int run_invalid_ad9361_read(uint16_t request_addr)
{
    struct pkt_buf packet = {
        .req = { 0x45, NIOS_PKT_16x64_TARGET_AD9361, 0, 0,
                 (uint8_t)request_addr, (uint8_t)(request_addr >> 8),
                 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
        .ready = false,
    };
    uint8_t target = 0;
    uint16_t addr = 0;
    uint64_t response = UINT64_MAX;
    bool write = true, success = true;
    unsigned int calls_before = read_calls;

    pkt_16x64(&packet);
    nios_pkt_16x64_resp_unpack(packet.resp, &target, &write, &addr,
                               &response, &success);
    return target == NIOS_PKT_16x64_TARGET_AD9361 && !write &&
           addr == request_addr && !success && read_calls == calls_before
        ? 0 : 1;
}

uint64_t rx_epoch_status_snapshot_read(void)
{
    epoch_snapshot_calls++;
    return UINT64_C(0x123456789abcdef0);
}

bool adi_spi_write_register_batch(uint16_t count, uint64_t data,
                                  uint8_t *completed)
{
    const uint64_t expected = UINT64_C(0x000e3d0228f0463b);
    calls++;

    if (count != 3 || data != expected || completed == NULL) {
        return false;
    }

    if (mode == BATCH_SUCCESS) {
        *completed = 3;
        return true;
    } else if (mode == BATCH_PARTIAL) {
        *completed = 1;
    } else {
        *completed = 0;
    }

    return false;
}

bool adi_spi_update_bits(uint16_t addr, uint8_t mask, uint8_t value,
                         uint8_t *result)
{
    update_calls++;
    if (addr != 0x23b || mask != 0x30 || value != 0x10 || result == NULL) {
        return false;
    }
    if (!update_success) {
        return false;
    }
    *result = 0x95;
    return true;
}

static int run_case(enum batch_mode requested_mode, bool expected_success,
                    uint64_t expected_completed)
{
    struct pkt_buf packet = {
        .req = { 0x45, 0x03, 0x01, 0x00, 0x03, 0x00,
                 0x3b, 0x46, 0xf0, 0x28, 0x02, 0x3d, 0x0e, 0x00,
                 0x00, 0x00 },
        .resp = { 0 },
        .ready = false,
    };
    uint8_t target = 0;
    uint16_t addr = 0;
    uint64_t completed = 0;
    bool write = false;
    bool success = false;

    mode = requested_mode;
    pkt_16x64(&packet);
    nios_pkt_16x64_resp_unpack(packet.resp, &target, &write, &addr,
                               &completed, &success);

    if (target != NIOS_PKT_16x64_TARGET_AD9361_WRITE_BATCH || !write ||
        addr != 3 || completed != expected_completed ||
        success != expected_success) {
        fprintf(stderr,
                "batch response mismatch: mode=%d target=%u write=%d "
                "addr=%u completed=%llu success=%d\n",
                requested_mode, target, write, addr,
                (unsigned long long)completed, success);
        return 1;
    }

    return 0;
}

static int run_malformed_batch_case(uint16_t count, uint64_t data)
{
    struct pkt_buf packet = make_16x64_packet(
        NIOS_PKT_16x64_TARGET_AD9361_WRITE_BATCH, true, count, data);
    uint8_t target = 0;
    uint16_t addr = 0;
    uint64_t response = 0;
    bool write = false;
    bool success = true;
    const unsigned int calls_before = calls;

    pkt_16x64(&packet);
    nios_pkt_16x64_resp_unpack(packet.resp, &target, &write, &addr,
                               &response, &success);

    return target == NIOS_PKT_16x64_TARGET_AD9361_WRITE_BATCH && write &&
           addr == count && !success && calls == calls_before ? 0 : 1;
}

static int run_update_bits_case(bool requested_success,
                                bool expected_success)
{
    struct pkt_buf packet = {
        .req = { 0x45, NIOS_PKT_16x64_TARGET_AD9361_UPDATE_BITS,
                 NIOS_PKT_16x64_FLAG_WRITE, 0x00, 0x3b, 0x02,
                 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x30, 0xa6,
                 0x00, 0x00 },
        .ready = false,
    };
    uint8_t target = 0;
    uint16_t addr = 0;
    uint64_t response = 0;
    bool write = false, success = false;

    update_success = requested_success;
    pkt_16x64(&packet);
    nios_pkt_16x64_resp_unpack(packet.resp, &target, &write, &addr,
                               &response, &success);

    if (target != NIOS_PKT_16x64_TARGET_AD9361_UPDATE_BITS || !write ||
        addr != 0x23b || success != expected_success ||
        (expected_success ? response != 0x95 :
         response != ((uint64_t)NIOS_PKT_16x64_UPDATE_BITS_ERROR_MARKER << 56))) {
        fprintf(stderr, "update-bits response mismatch: target=%u write=%u "
                        "addr=0x%x data=0x%016llx success=%u\n",
                target, write, addr, (unsigned long long)response, success);
        return 1;
    }
    return 0;
}

static int run_update_bits_value_outside_mask(void)
{
    struct pkt_buf packet = {
        .req = { 0x45, NIOS_PKT_16x64_TARGET_AD9361_UPDATE_BITS,
                 NIOS_PKT_16x64_FLAG_WRITE, 0x00, 0x3b, 0x02,
                 0x00, 0x00, 0x00, 0x00, 0x00, 0x11, 0x30, 0xa6,
                 0x00, 0x00 },
        .ready = false,
    };
    uint64_t response = 0;
    bool success = true;
    unsigned int calls_before = update_calls;

    pkt_16x64(&packet);
    nios_pkt_16x64_resp_unpack(packet.resp, NULL, NULL, NULL, &response,
                               &success);
    return !success &&
           response == ((uint64_t)NIOS_PKT_16x64_UPDATE_BITS_ERROR_MARKER
                        << 56) &&
           update_calls == calls_before ? 0 : 1;
}

static int run_epoch_snapshot_case(uint16_t request_addr,
                                   bool expected_success,
                                   unsigned int expected_calls)
{
    struct pkt_buf packet = {
        .req = { 0x45, NIOS_PKT_16x64_TARGET_RX_EPOCH_SNAPSHOT,
                 0, 0, (uint8_t)request_addr,
                 (uint8_t)(request_addr >> 8),
                 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
        .ready = false,
    };
    uint8_t target = 0;
    uint16_t addr = 0;
    uint64_t response = 0;
    bool write = true, success = false;

    pkt_16x64(&packet);
    nios_pkt_16x64_resp_unpack(packet.resp, &target, &write, &addr,
                               &response, &success);
    if (target != NIOS_PKT_16x64_TARGET_RX_EPOCH_SNAPSHOT || write ||
        addr != request_addr || success != expected_success ||
        epoch_snapshot_calls != expected_calls ||
        (expected_success &&
         response != UINT64_C(0x123456789abcdef0)) ||
        (!expected_success && response != 0)) {
        fprintf(stderr, "epoch snapshot response mismatch: target=%u "
                        "write=%u addr=0x%x data=0x%016llx success=%u\n",
                target, write, addr, (unsigned long long)response, success);
        return 1;
    }
    return 0;
}

static int run_read_case(bool requested_success, bool expected_success)
{
    struct pkt_buf packet = {
        .req = { 0x45, NIOS_PKT_16x64_TARGET_AD9361, 0, 0,
                 0x47, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
        .ready = false,
    };
    uint8_t target = 0;
    uint16_t addr = 0;
    uint64_t response = UINT64_MAX;
    bool write = true, success = false;

    read_success = requested_success;
    pkt_16x64(&packet);
    nios_pkt_16x64_resp_unpack(packet.resp, &target, &write, &addr,
                               &response, &success);

    if (target != NIOS_PKT_16x64_TARGET_AD9361 || write || addr != 0x247 ||
        success != expected_success ||
        (expected_success && response != UINT64_C(0x0200000000000000))) {
        fprintf(stderr, "AD9361 read response mismatch: success=%u data=%016llx\n",
                success, (unsigned long long)response);
        return 1;
    }
    return 0;
}

int main(void)
{
    if (run_case(BATCH_SUCCESS, true, 3) ||
        run_case(BATCH_FAILURE, false, 0) ||
        run_case(BATCH_PARTIAL, false, 1) || calls != 3 ||
        run_malformed_batch_case(0, 0) ||
        run_malformed_batch_case(4, 0) ||
        run_malformed_batch_case(1, UINT64_C(1) << 18) ||
        run_malformed_batch_case(3, UINT64_C(1) << 54) ||
        run_gain_row_case(UINT64_C(0x44332211), true, 1) ||
        run_gain_row_case(UINT64_C(0x1000044332211), false, 1) ||
        run_update_bits_case(true, true) ||
        run_update_bits_case(false, false) || update_calls != 2 ||
        run_ad9361_write(false, false) ||
        run_ad9361_write(true, true) || write_calls != 2 ||
        run_update_bits_value_outside_mask() || update_calls != 2 ||
        run_epoch_snapshot_case(0, true, 1) ||
        run_epoch_snapshot_case(1, false, 1) ||
        run_read_case(true, true) ||
        run_read_case(false, false) || read_calls != 2 ||
        run_invalid_ad9361_read(0x8247) ||
        run_invalid_ad9361_read(0x0c47) || read_calls != 2) {
        return 1;
    }

    puts("NIOS RFPLL batch and atomic RMW packet paths: PASS");
    return 0;
}
