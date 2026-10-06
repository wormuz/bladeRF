#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pkt_handler.h"
#include "pkt_16x64.h"
#include "devices.h"

enum batch_mode {
    BATCH_SUCCESS,
    BATCH_FAILURE,
    BATCH_PARTIAL,
};

static enum batch_mode mode;
static unsigned int calls;
static unsigned int update_calls;
static bool update_success = true;

uint64_t rx_epoch_status_snapshot_read(void)
{
    return UINT64_C(0x123456789abcdef0);
}

bool adi_spi_write_register_batch(uint16_t count, uint64_t data,
                                  uint8_t *completed)
{
    const uint64_t expected = UINT64_C(0x00ce3d0228f0463b);
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
                 0x3b, 0x46, 0xf0, 0x28, 0x02, 0x3d, 0xce, 0x00,
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

static int run_epoch_snapshot_case(void)
{
    struct pkt_buf packet = {
        .req = { 0x45, NIOS_PKT_16x64_TARGET_RX_EPOCH_SNAPSHOT,
                 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
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
        addr != 0 || response != UINT64_C(0x123456789abcdef0) || !success) {
        fprintf(stderr, "epoch snapshot response mismatch: target=%u "
                        "write=%u addr=0x%x data=0x%016llx success=%u\n",
                target, write, addr, (unsigned long long)response, success);
        return 1;
    }
    return 0;
}

int main(void)
{
    if (run_case(BATCH_SUCCESS, true, 3) ||
        run_case(BATCH_FAILURE, false, 0) ||
        run_case(BATCH_PARTIAL, false, 1) || calls != 3 ||
        run_update_bits_case(true, true) ||
        run_update_bits_case(false, false) || update_calls != 2 ||
        run_epoch_snapshot_case()) {
        return 1;
    }

    puts("NIOS RFPLL batch and atomic RMW packet paths: PASS");
    return 0;
}
