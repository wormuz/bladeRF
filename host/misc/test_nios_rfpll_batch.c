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

int main(void)
{
    if (run_case(BATCH_SUCCESS, true, 3) ||
        run_case(BATCH_FAILURE, false, 0) ||
        run_case(BATCH_PARTIAL, false, 1) || calls != 3) {
        return 1;
    }

    puts("NIOS RFPLL batch packet success/failure/partial: PASS");
    return 0;
}
