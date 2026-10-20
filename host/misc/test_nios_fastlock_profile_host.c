#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "board/board.h"
#include "backend/usb/usb.h"
#include "backend/usb/nios_access.h"
#include "nios_pkt_16x64.h"
#include "nios_pkt_formats.h"

/* Host side of NIOS_PKT_16x64_TARGET_FASTLOCK_PROFILE: packet layout,
 * the bounded write sequence, and response-to-status mapping. The mock
 * implements the NIOS staging contract for one direction/slot table. */

enum nios_mode {
    NIOS_EMULATE,
    NIOS_OLD_FIRMWARE_ECHO,
    NIOS_SLOT_QUEUED,
    NIOS_SECOND_READ_FAILS,
};

static enum nios_mode nios_mode;
static uint8_t request[NIOS_PKT_LEN];
static uint16_t addr_log[8];
static unsigned int out_count;
static uint8_t slots[2][16][16];
static bool slot_valid[2][16];
static uint8_t staged[16];
static uint8_t staged_halves;

static int mock_bulk_transfer(void *driver, uint8_t endpoint, void *buffer,
                              uint32_t length, uint32_t timeout_ms,
                              uint32_t *transferred,
                              bool *transferred_valid)
{
    uint8_t *bytes = buffer;
    uint8_t target = 0, slot = 0, op = 0;
    uint16_t addr = 0;
    uint64_t data = 0;
    bool write = false, tx = false, success = true;

    (void)driver;
    (void)timeout_ms;
    if (length != NIOS_PKT_LEN) {
        return BLADERF_ERR_UNEXPECTED;
    }
    if (transferred != NULL) {
        *transferred = length;
    }
    if (transferred_valid != NULL) {
        *transferred_valid = true;
    }

    if (endpoint == PERIPHERAL_EP_OUT) {
        memcpy(request, bytes, NIOS_PKT_LEN);
        if (out_count < sizeof(addr_log) / sizeof(addr_log[0])) {
            nios_pkt_16x64_unpack(bytes, NULL, NULL, &addr_log[out_count],
                                  NULL);
        }
        out_count++;
        return 0;
    }
    if (endpoint != PERIPHERAL_EP_IN) {
        return BLADERF_ERR_UNEXPECTED;
    }

    nios_pkt_16x64_unpack(request, &target, &write, &addr, &data);
    if (request[NIOS_PKT_16x64_IDX_MAGIC] != NIOS_PKT_16x64_MAGIC ||
        target != NIOS_PKT_16x64_TARGET_FASTLOCK_PROFILE ||
        !nios_pkt_16x64_fastlock_profile_addr_unpack(addr, &tx, &slot,
                                                     &op) ||
        slot >= 16) {
        return BLADERF_ERR_UNEXPECTED;
    }

    if (nios_mode == NIOS_OLD_FIRMWARE_ECHO) {
        success = false;
    } else if (write && op != NIOS_PKT_16x64_FASTLOCK_PROFILE_OP_COMMIT) {
        nios_pkt_16x64_fastlock_profile_half_unpack(data, &staged[8u * op]);
        staged_halves |= (uint8_t)(1u << op);
        data = 0;
    } else if (write) {
        if (nios_mode == NIOS_SLOT_QUEUED) {
            success = false;
            data = nios_pkt_16x64_fastlock_profile_error(
                NIOS_PKT_16x64_FASTLOCK_PROFILE_ERR_QUEUED);
        } else if (staged_halves != 3 || data != 0) {
            success = false;
            data = nios_pkt_16x64_fastlock_profile_error(
                NIOS_PKT_16x64_FASTLOCK_PROFILE_ERR_NOT_STAGED);
        } else {
            memcpy(slots[tx][slot], staged, sizeof(staged));
            slot_valid[tx][slot] = true;
            data = 0;
        }
        staged_halves = 0;
    } else if (op == NIOS_PKT_16x64_FASTLOCK_PROFILE_OP_COMMIT) {
        data = slot_valid[tx][slot] ? 1u | (3u << 8) : 0;
    } else if (!slot_valid[tx][slot] ||
               (nios_mode == NIOS_SECOND_READ_FAILS &&
                op == NIOS_PKT_16x64_FASTLOCK_PROFILE_OP_HALF1)) {
        success = false;
        data = nios_pkt_16x64_fastlock_profile_error(
            NIOS_PKT_16x64_FASTLOCK_PROFILE_ERR_EMPTY);
    } else {
        data = nios_pkt_16x64_fastlock_profile_half_pack(
            &slots[tx][slot][8u * op]);
    }

    nios_pkt_16x64_resp_pack(bytes, target, write, addr, data, success);
    return 0;
}

static const struct usb_fns mock_usb_fns = {
    .bulk_transfer = mock_bulk_transfer,
};

void log_error(const char *format, ...)
{
    (void)format;
}

void log_debug(const char *format, ...)
{
    (void)format;
}

const char *bladerf_strerror(int error)
{
    (void)error;
    return "mock";
}

static int check_packet_layout(void)
{
    uint8_t buf[NIOS_PKT_LEN];
    const uint8_t words[8] = { 0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76,
                               0x87 };
    uint8_t target = 0, slot = 0, op = 0;
    uint16_t addr = 0;
    uint64_t data = 0;
    bool write = false, tx = false;

    nios_pkt_16x64_pack(
        buf, NIOS_PKT_16x64_TARGET_FASTLOCK_PROFILE, true,
        nios_pkt_16x64_fastlock_profile_addr(
            true, 9, NIOS_PKT_16x64_FASTLOCK_PROFILE_OP_HALF1),
        nios_pkt_16x64_fastlock_profile_half_pack(words));
    nios_pkt_16x64_unpack(buf, &target, &write, &addr, &data);

    /* Bytes 4..5 carry addr LE; bytes 6..13 carry profile words 8..15 in
     * order, so the wire image equals the word order. */
    if (buf[NIOS_PKT_16x64_IDX_MAGIC] != 'E' ||
        buf[NIOS_PKT_16x64_IDX_TARGET_ID] != 0x06 ||
        buf[NIOS_PKT_16x64_IDX_FLAGS] != NIOS_PKT_16x64_FLAG_WRITE ||
        buf[NIOS_PKT_16x64_IDX_ADDR] != 0x09 ||
        buf[NIOS_PKT_16x64_IDX_ADDR + 1] != 0x05 ||
        memcmp(&buf[NIOS_PKT_16x64_IDX_DATA], words, sizeof(words)) != 0 ||
        target != NIOS_PKT_16x64_TARGET_FASTLOCK_PROFILE || !write ||
        !nios_pkt_16x64_fastlock_profile_addr_unpack(addr, &tx, &slot,
                                                     &op) ||
        !tx || slot != 9 || op != NIOS_PKT_16x64_FASTLOCK_PROFILE_OP_HALF1 ||
        nios_pkt_16x64_fastlock_profile_addr_unpack(0x0e00, NULL, NULL,
                                                    NULL) ||
        nios_pkt_16x64_fastlock_profile_addr_unpack(0x1000, NULL, NULL,
                                                    NULL) ||
        nios_pkt_16x64_fastlock_profile_addr_unpack(0x0200, NULL, NULL,
                                                    NULL) ||
        nios_pkt_16x64_fastlock_profile_error_code(
            nios_pkt_16x64_fastlock_profile_error(
                NIOS_PKT_16x64_FASTLOCK_PROFILE_ERR_LOADING)) !=
            NIOS_PKT_16x64_FASTLOCK_PROFILE_ERR_LOADING ||
        nios_pkt_16x64_fastlock_profile_error_code(data) != -1) {
        fputs("fastlock profile packet layout mismatch\n", stderr);
        return 1;
    }
    return 0;
}

static int run(enum nios_mode mode, int (*body)(struct bladerf *))
{
    struct bladerf_usb usb = { .fn = &mock_usb_fns, .driver = NULL };
    usb.peripheral = &usb.peripheral_local;
    struct bladerf dev = { .backend_data = &usb };
    int result;

    MUTEX_INIT(&usb.peripheral_local.lock);
    nios_mode = mode;
    out_count = 0;
    staged_halves = 0;
    result = body(&dev);
    MUTEX_DESTROY(&usb.peripheral_local.lock);
    return result;
}

static const uint8_t test_words[16] = {
    0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
    0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
};

static int body_round_trip(struct bladerf *dev)
{
    uint8_t words[16];
    uint8_t state = 0, rffe = 0;

    if (nios_fastlock_profile_write(dev, true, 12, test_words) != 0 ||
        out_count != 3 ||
        addr_log[0] != nios_pkt_16x64_fastlock_profile_addr(true, 12, 0) ||
        addr_log[1] != nios_pkt_16x64_fastlock_profile_addr(true, 12, 1) ||
        addr_log[2] != nios_pkt_16x64_fastlock_profile_addr(true, 12, 2) ||
        memcmp(slots[1][12], test_words, sizeof(test_words)) != 0 ||
        slot_valid[0][12]) {
        fputs("fastlock profile write sequence mismatch\n", stderr);
        return 1;
    }

    out_count = 0;
    memset(words, 0, sizeof(words));
    if (nios_fastlock_profile_read(dev, true, 12, words) != 0 ||
        out_count != 2 ||
        memcmp(words, test_words, sizeof(words)) != 0 ||
        nios_fastlock_profile_state(dev, true, 12, &state, &rffe) != 0 ||
        state != 1 || rffe != 3) {
        fputs("fastlock profile read mismatch\n", stderr);
        return 1;
    }
    return 0;
}

static int body_old_firmware(struct bladerf *dev)
{
    uint8_t words[16];

    if (nios_fastlock_profile_write(dev, false, 0, test_words) !=
            BLADERF_ERR_UNSUPPORTED ||
        out_count != 1 ||
        nios_fastlock_profile_read(dev, false, 0, words) !=
            BLADERF_ERR_UNSUPPORTED) {
        fputs("old firmware echo not reported unsupported\n", stderr);
        return 1;
    }
    return 0;
}

static int body_slot_queued(struct bladerf *dev)
{
    if (nios_fastlock_profile_write(dev, false, 4, test_words) !=
            BLADERF_ERR_WOULD_BLOCK ||
        out_count != 3 || slot_valid[0][4]) {
        fputs("queued NIOS slot not reported would-block\n", stderr);
        return 1;
    }
    return 0;
}

static int body_partial_read(struct bladerf *dev)
{
    uint8_t words[16];
    uint8_t untouched[16];

    memset(words, 0xa5, sizeof(words));
    memcpy(untouched, words, sizeof(words));
    if (nios_fastlock_profile_read(dev, true, 12, words) !=
            BLADERF_ERR_INVAL ||
        memcmp(words, untouched, sizeof(words)) != 0 ||
        nios_fastlock_profile_read(dev, true, 13, words) !=
            BLADERF_ERR_INVAL) {
        fputs("failed profile read published partial words\n", stderr);
        return 1;
    }
    return 0;
}

int main(void)
{
    if (check_packet_layout() != 0 ||
        run(NIOS_EMULATE, body_round_trip) != 0 ||
        run(NIOS_OLD_FIRMWARE_ECHO, body_old_firmware) != 0 ||
        run(NIOS_SLOT_QUEUED, body_slot_queued) != 0 ||
        run(NIOS_SECOND_READ_FAILS, body_partial_read) != 0) {
        return 1;
    }
    puts("NIOS fastlock profile host packets: PASS");
    return 0;
}
