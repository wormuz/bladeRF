#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "board/board.h"
#include "backend/usb/usb.h"
#include "backend/usb/nios_access.h"
#include "nios_pkt_16x64.h"
#include "nios_pkt_formats.h"

enum response_mode {
    RESPONSE_SUCCESS,
    RESPONSE_OLD_FIRMWARE_ECHO,
    RESPONSE_PARTIAL_FAILURE,
    RESPONSE_UPDATE_SUCCESS,
    RESPONSE_UPDATE_OLD_FIRMWARE_ECHO,
    RESPONSE_UPDATE_FAILURE,
    RESPONSE_SNAPSHOT_SUCCESS,
    RESPONSE_SNAPSHOT_UNSUPPORTED,
};

static enum response_mode response_mode;
static uint8_t request[NIOS_PKT_LEN];
static unsigned int out_count;
static unsigned int in_count;

static int mock_bulk_transfer(void *driver, uint8_t endpoint, void *buffer,
                              uint32_t length, uint32_t timeout_ms)
{
    uint8_t *bytes = buffer;
    uint8_t target;
    bool write;
    uint16_t count;
    uint64_t data;
    (void)driver;
    (void)timeout_ms;

    if (length != NIOS_PKT_LEN) {
        return BLADERF_ERR_UNEXPECTED;
    }

    if (endpoint == PERIPHERAL_EP_OUT) {
        memcpy(request, bytes, length);
        out_count++;
        return 0;
    }

    if (endpoint != PERIPHERAL_EP_IN) {
        return BLADERF_ERR_UNEXPECTED;
    }

    nios_pkt_16x64_unpack(request, &target, &write, &count, &data);
    in_count++;
    if (target == NIOS_PKT_16x64_TARGET_RX_EPOCH_SNAPSHOT && !write) {
        if (response_mode == RESPONSE_SNAPSHOT_SUCCESS) {
            nios_pkt_16x64_resp_pack(bytes, target, false, count,
                                     UINT64_C(0x10203040a0b0c0d0), true);
        } else if (response_mode == RESPONSE_SNAPSHOT_UNSUPPORTED) {
            nios_pkt_16x64_resp_pack(bytes, target, false, count, 0, false);
        } else {
            return BLADERF_ERR_UNEXPECTED;
        }
        return 0;
    }
    if (!write) {
        return BLADERF_ERR_UNEXPECTED;
    }
    if (target == NIOS_PKT_16x64_TARGET_AD9361_WRITE_BATCH &&
        response_mode <= RESPONSE_PARTIAL_FAILURE) {
        if (response_mode == RESPONSE_SUCCESS) {
            nios_pkt_16x64_resp_pack(bytes, target, true, count, count, true);
        } else if (response_mode == RESPONSE_OLD_FIRMWARE_ECHO) {
            nios_pkt_16x64_resp_pack(bytes, target, true, count, data, false);
        } else {
            nios_pkt_16x64_resp_pack(bytes, target, true, count, 1, false);
        }
    } else if (target == NIOS_PKT_16x64_TARGET_AD9361_UPDATE_BITS &&
               response_mode >= RESPONSE_UPDATE_SUCCESS) {
        const uint64_t expected = NIOS_PKT_16x64_UPDATE_BITS_REQUEST(
            NIOS_PKT_16x64_UPDATE_BITS_MARKER, 0x30, 0x10);
        if (count != 0x23b || data != expected) {
            return BLADERF_ERR_UNEXPECTED;
        }
        if (response_mode == RESPONSE_UPDATE_SUCCESS) {
            nios_pkt_16x64_resp_pack(bytes, target, true, count, 0x95, true);
        } else if (response_mode == RESPONSE_UPDATE_OLD_FIRMWARE_ECHO) {
            nios_pkt_16x64_resp_pack(bytes, target, true, count, data, false);
        } else {
            nios_pkt_16x64_resp_pack(bytes, target, true, count,
                (uint64_t)NIOS_PKT_16x64_UPDATE_BITS_ERROR_MARKER << 56,
                false);
        }
    } else {
        return BLADERF_ERR_UNEXPECTED;
    }

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

static int expect(enum response_mode mode, int expected_status)
{
    struct bladerf_usb usb = { .fn = &mock_usb_fns, .driver = NULL };
    struct bladerf dev = { .backend_data = &usb };
    const uint16_t regs[] = { 0x23b, 0x23c, 0x23d };
    const uint8_t values[] = { 0x11, 0x22, 0x33 };
    int status;

    response_mode = mode;
    status = nios_ad9361_spi_write_batch(&dev, regs, values, 3);
    if (status != expected_status) {
        fprintf(stderr, "status mismatch: mode=%d got=%d expected=%d\n",
                mode, status, expected_status);
        return 1;
    }

    return 0;
}

static int expect_update(enum response_mode mode, int expected_status)
{
    struct bladerf_usb usb = { .fn = &mock_usb_fns, .driver = NULL };
    struct bladerf dev = { .backend_data = &usb };
    int status;

    response_mode = mode;
    status = nios_ad9361_spi_update_bits(&dev, 0x23b, 0x30, 0x10);
    if (status != expected_status) {
        fprintf(stderr, "RMW status mismatch: mode=%d got=%d expected=%d\n",
                mode, status, expected_status);
        return 1;
    }
    return 0;
}

static int expect_snapshot(enum response_mode mode, int expected_status)
{
    struct bladerf_usb usb = { .fn = &mock_usb_fns, .driver = NULL };
    struct bladerf dev = { .backend_data = &usb };
    uint32_t status_word = 0, timestamp_lo = 0;
    int status;

    response_mode = mode;
    status = nios_rx_epoch_status_snapshot_read(&dev, &status_word,
                                                &timestamp_lo);
    if (status != expected_status ||
        (expected_status == 0 &&
         (status_word != 0x10203040 || timestamp_lo != 0xa0b0c0d0))) {
        fprintf(stderr, "snapshot mismatch: mode=%d status=%d "
                        "status_word=0x%08x timestamp=0x%08x\n",
                mode, status, status_word, timestamp_lo);
        return 1;
    }
    return 0;
}

int main(void)
{
    if (expect(RESPONSE_SUCCESS, 0) ||
        expect(RESPONSE_OLD_FIRMWARE_ECHO, BLADERF_ERR_UNSUPPORTED) ||
        expect(RESPONSE_PARTIAL_FAILURE, BLADERF_ERR_FPGA_OP) ||
        expect_update(RESPONSE_UPDATE_SUCCESS, 0) ||
        expect_update(RESPONSE_UPDATE_OLD_FIRMWARE_ECHO,
                      BLADERF_ERR_UNSUPPORTED) ||
        expect_update(RESPONSE_UPDATE_FAILURE, BLADERF_ERR_FPGA_OP) ||
        expect_snapshot(RESPONSE_SNAPSHOT_SUCCESS, 0) ||
        expect_snapshot(RESPONSE_SNAPSHOT_UNSUPPORTED,
                        BLADERF_ERR_UNSUPPORTED) ||
        out_count != 8 || in_count != 8) {
        return 1;
    }

    puts("libbladeRF RFPLL batch and atomic RMW mappings: PASS");
    return 0;
}
