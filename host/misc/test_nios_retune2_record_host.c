#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "board/board.h"
#include "backend/usb/usb.h"
#include "backend/usb/nios_access.h"
#include "nios_pkt_16x64.h"
#include "nios_pkt_formats.h"

/* Host side of NIOS_PKT_16x64_TARGET_RETUNE2_RECORD: transactions per
 * record, field decoding and error mapping. The mock serves one success
 * and one failure record the way pkt_retune2_record_read() does. */

enum mock_mode {
    MOCK_RECORDS,
    MOCK_DURATION,
    MOCK_OLD_FIRMWARE_ECHO,
    MOCK_OVERWRITTEN_BEFORE_SECOND_WORD,
    MOCK_UNKNOWN_FLAG,
    MOCK_UNKNOWN_SYNTH_FLAG,
};

static enum mock_mode mock_mode;
static uint8_t request[NIOS_PKT_LEN];
static unsigned int transactions;

static const uint32_t success_sequence = 41;
static const uint32_t failure_sequence = 7;

static bool serve(uint16_t addr, uint64_t sequence, uint64_t *data)
{
    bool failure = false, tx = false;
    uint8_t word = 0;

    if (!nios_pkt_16x64_retune2_record_addr_unpack(addr, &failure, &tx,
                                                   &word) || tx) {
        *data = nios_pkt_16x64_retune2_record_error(
            NIOS_PKT_16x64_RETUNE2_RECORD_ERR_INVALID);
        return false;
    }
    if (word == NIOS_PKT_16x64_RETUNE2_RECORD_WORD_COUNTS) {
        *data = 300u | ((uint64_t)12u << 32);
        return true;
    }
    if (failure) {
        if (sequence != failure_sequence) {
            *data = nios_pkt_16x64_retune2_record_error(
                NIOS_PKT_16x64_RETUNE2_RECORD_ERR_SEQUENCE);
            return false;
        }
        *data = nios_pkt_16x64_retune2_record_identity(0xbeef, 11, 0, 0x37,
                                                       9);
        return true;
    }
    if (sequence != success_sequence ||
        (mock_mode == MOCK_OVERWRITTEN_BEFORE_SECOND_WORD &&
         word == NIOS_PKT_16x64_RETUNE2_RECORD_WORD_EXECUTION)) {
        *data = nios_pkt_16x64_retune2_record_error(
            NIOS_PKT_16x64_RETUNE2_RECORD_ERR_SEQUENCE);
        return false;
    }
    if (word == NIOS_PKT_16x64_RETUNE2_RECORD_WORD_IDENTITY) {
        *data = nios_pkt_16x64_retune2_record_identity(
            0x1234, 9,
            mock_mode == MOCK_UNKNOWN_FLAG
                ? 0x80
                : (NIOS_PKT_RETUNE2_EXEC_TS_VALID |
                   NIOS_PKT_RETUNE2_EXEC_PLL_LOCKED),
            0x4d, 0);
    } else if (word == NIOS_PKT_16x64_RETUNE2_RECORD_WORD_SYNTH) {
        /* flags, then synthesizer bytes 0x11..0x77 */
        *data = (mock_mode == MOCK_UNKNOWN_SYNTH_FLAG
                     ? 0x80u
                     : (NIOS_PKT_RETUNE2_EXEC_TS_VALID |
                        NIOS_PKT_RETUNE2_EXEC_PLL_LOCKED)) |
                UINT64_C(0x7766554433221100);
    } else {
        uint32_t elapsed = mock_mode == MOCK_DURATION
            ? (NIOS_PKT_16x64_RETUNE2_NOW_DURATION_VALID | 1234567u)
            : 1234567u;
        *data = (uint64_t)elapsed |
                ((uint64_t)(UINT64_C(2400000000) >> 1) << 32);
    }
    return true;
}

static int mock_bulk_transfer(void *driver, uint8_t endpoint, void *buffer,
                              uint32_t length, uint32_t timeout_ms,
                              uint32_t *transferred,
                              bool *transferred_valid)
{
    uint8_t *bytes = buffer;
    uint8_t target = 0;
    uint16_t addr = 0;
    uint64_t data = 0;
    bool write = false, success;

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
        transactions++;
        return 0;
    }
    nios_pkt_16x64_unpack(request, &target, &write, &addr, &data);
    if (request[NIOS_PKT_16x64_IDX_MAGIC] != NIOS_PKT_16x64_MAGIC ||
        target != NIOS_PKT_16x64_TARGET_RETUNE2_RECORD || write) {
        return BLADERF_ERR_UNEXPECTED;
    }
    if (mock_mode == MOCK_OLD_FIRMWARE_ECHO) {
        success = false;
    } else {
        success = serve(addr, data, &data);
    }
    nios_pkt_16x64_resp_pack(bytes, target, false, addr, data, success);
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

static int failures;

#define CHECK(cond, ...)                         \
    do {                                         \
        if (!(cond)) {                           \
            fprintf(stderr, __VA_ARGS__);        \
            fputc('\n', stderr);                 \
            failures++;                          \
        }                                        \
    } while (0)

int main(void)
{
    struct bladerf_usb usb = { .fn = &mock_usb_fns, .driver = NULL };
    usb.peripheral = &usb.peripheral_local;
    struct bladerf dev = { .backend_data = &usb };
    struct nios_retune2_record record;
    uint32_t successes = 0, failure_count = 0;
    int status;

    MUTEX_INIT(&usb.peripheral_local.lock);

    mock_mode = MOCK_RECORDS;
    transactions = 0;
    status = nios_retune2_record_counts(&dev, BLADERF_MODULE_RX, &successes,
                                        &failure_count);
    CHECK(status == 0 && successes == 300 && failure_count == 12 &&
              transactions == 1, "counts: status %d tx %u", status,
          transactions);

    transactions = 0;
    status = nios_retune2_record_read(&dev, BLADERF_MODULE_RX, false,
                                      success_sequence, false, &record);
    CHECK(status == 0 && transactions == 2 &&
              record.request_token == 0x1234 && record.nios_profile == 9 &&
              record.execution_flags == (NIOS_PKT_RETUNE2_EXEC_TS_VALID |
                                         NIOS_PKT_RETUNE2_EXEC_PLL_LOCKED) &&
              record.session_generation_low == 0x4d &&
              record.executed_delta == 1234567 &&
              record.readback_frequency_hz == UINT64_C(2400000000),
          "success record: status %d tx %u", status, transactions);

    mock_mode = MOCK_DURATION;
    {
        uint32_t duration_ticks = 0;
        bool duration_valid = false;

        transactions = 0;
        status = nios_retune2_completion_device_duration_read(
            &dev, BLADERF_MODULE_RX, success_sequence, &duration_ticks,
            &duration_valid);
        CHECK(status == 0 && duration_valid && duration_ticks == 1234567 &&
                  transactions == 1,
              "NOW device duration: status %d valid %d ticks %u tx %u",
              status, duration_valid, duration_ticks, transactions);
    }
    mock_mode = MOCK_RECORDS;

    transactions = 0;
    status = nios_retune2_record_read(&dev, BLADERF_MODULE_RX, true,
                                      failure_sequence, false, &record);
    CHECK(status == 0 && transactions == 1 &&
              record.request_token == 0xbeef && record.nios_profile == 11 &&
              record.failure_stage == 9 &&
              record.session_generation_low == 0x37 &&
              record.executed_delta == 0,
          "failure record: status %d tx %u", status, transactions);

    /* FPGA >= 0.33.0: a third read returns the synthesizer bytes; a failure
     * record has none and still costs one read. */
    transactions = 0;
    status = nios_retune2_record_read(&dev, BLADERF_MODULE_RX, false,
                                      success_sequence, true, &record);
    CHECK(status == 0 && transactions == 3 && record.synth_read &&
              record.synth[0] == 0x11 && record.synth[5] == 0x66 &&
              record.synth[NIOS_PKT_RETUNE2_SYNTH_SETUP] == 0x77 &&
              record.readback_frequency_hz == UINT64_C(2400000000),
          "success record with synth: status %d tx %u", status,
          transactions);
    transactions = 0;
    status = nios_retune2_record_read(&dev, BLADERF_MODULE_RX, true,
                                      failure_sequence, true, &record);
    CHECK(status == 0 && transactions == 1 && !record.synth_read,
          "failure record with synth: status %d tx %u", status,
          transactions);
    mock_mode = MOCK_UNKNOWN_SYNTH_FLAG;
    status = nios_retune2_record_read(&dev, BLADERF_MODULE_RX, false,
                                      success_sequence, true, &record);
    CHECK(status == BLADERF_ERR_UNEXPECTED, "unknown synth flag: %d", status);
    mock_mode = MOCK_RECORDS;

    status = nios_retune2_record_read(&dev, BLADERF_MODULE_RX, false,
                                      success_sequence + 16, false, &record);
    CHECK(status == BLADERF_ERR_WOULD_BLOCK && record.request_token == 0,
          "overwritten record: %d", status);

    mock_mode = MOCK_OVERWRITTEN_BEFORE_SECOND_WORD;
    status = nios_retune2_record_read(&dev, BLADERF_MODULE_RX, false,
                                      success_sequence, false, &record);
    CHECK(status == BLADERF_ERR_WOULD_BLOCK && record.request_token == 0,
          "record torn between words was accepted: %d", status);

    mock_mode = MOCK_UNKNOWN_FLAG;
    status = nios_retune2_record_read(&dev, BLADERF_MODULE_RX, false,
                                      success_sequence, false, &record);
    CHECK(status == BLADERF_ERR_UNEXPECTED, "unknown flag: %d", status);

    mock_mode = MOCK_OLD_FIRMWARE_ECHO;
    status = nios_retune2_record_counts(&dev, BLADERF_MODULE_RX, &successes,
                                        &failure_count);
    CHECK(status == BLADERF_ERR_UNSUPPORTED, "old firmware: %d", status);

    MUTEX_DESTROY(&usb.peripheral_local.lock);
    if (failures != 0) {
        return 1;
    }
    puts("NIOS retune2 record reads: PASS");
    return 0;
}
