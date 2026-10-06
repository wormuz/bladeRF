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
    if (target != NIOS_PKT_16x64_TARGET_AD9361_WRITE_BATCH || !write) {
        return BLADERF_ERR_UNEXPECTED;
    }

    in_count++;
    if (response_mode == RESPONSE_SUCCESS) {
        nios_pkt_16x64_resp_pack(bytes, target, true, count, count, true);
    } else if (response_mode == RESPONSE_OLD_FIRMWARE_ECHO) {
        nios_pkt_16x64_resp_pack(bytes, target, true, count, data, false);
    } else {
        nios_pkt_16x64_resp_pack(bytes, target, true, count, 1, false);
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

int main(void)
{
    if (expect(RESPONSE_SUCCESS, 0) ||
        expect(RESPONSE_OLD_FIRMWARE_ECHO, BLADERF_ERR_UNSUPPORTED) ||
        expect(RESPONSE_PARTIAL_FAILURE, BLADERF_ERR_FPGA_OP) ||
        out_count != 3 || in_count != 3) {
        return 1;
    }

    puts("libbladeRF RFPLL batch success/old-firmware/partial: PASS");
    return 0;
}
