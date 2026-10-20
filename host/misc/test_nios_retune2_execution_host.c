#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "board/board.h"
#include "backend/usb/usb.h"
#include "backend/usb/nios_access.h"
#include "nios_pkt_8x32.h"
#include "nios_pkt_formats.h"

/* Host side of the retune2 success-record execution words (FPGA 0.32.0):
 * read order, sequence bracketing, flag validation and capability gating.
 * The mock serves one NIOS completion record over 8x32 packets. */

enum mock_mode {
    MOCK_RECORD,
    MOCK_SEQUENCE_CHANGES,
    MOCK_UNKNOWN_FLAG,
};

static enum mock_mode mock_mode;
static uint8_t request[NIOS_PKT_LEN];
static uint32_t selected_sequence;
static uint8_t selected_addr = 0xff;
static unsigned int word_reads;
static uint8_t highest_word;
static uint64_t mock_capabilities;

static const uint32_t record_sequence = 41;

static uint32_t record_word(uint8_t word)
{
    switch (word) {
        case NIOS_PKT_8x32_RETUNE2_COMPLETION_SEQUENCE:
            if (mock_mode == MOCK_SEQUENCE_CHANGES && word_reads > 1) {
                return record_sequence + 16u;
            }
            return record_sequence;
        case NIOS_PKT_8x32_RETUNE2_COMPLETION_FREQ_LO:
            return UINT32_C(2400000000);
        case NIOS_PKT_8x32_RETUNE2_COMPLETION_FREQ_HI:
            return 0;
        case NIOS_PKT_8x32_RETUNE2_COMPLETION_PROFILE:
            return 9;
        case NIOS_PKT_8x32_RETUNE2_COMPLETION_TS_LO:
            return UINT32_C(0x89abcdef);
        case NIOS_PKT_8x32_RETUNE2_COMPLETION_TS_HI:
            return 0x12;
        case NIOS_PKT_8x32_RETUNE2_COMPLETION_SESSION_GENERATION:
            return 77;
        case NIOS_PKT_8x32_RETUNE2_COMPLETION_REQUEST_TOKEN:
            return 0x1234;
        case NIOS_PKT_8x32_RETUNE2_COMPLETION_EXEC_TS_LO:
            return UINT32_C(0x89abd000);
        case NIOS_PKT_8x32_RETUNE2_COMPLETION_EXEC_TS_HI:
            return 0x12;
        case NIOS_PKT_8x32_RETUNE2_COMPLETION_EXEC_FLAGS:
            return mock_mode == MOCK_UNKNOWN_FLAG
                ? 0x7u
                : (NIOS_PKT_8x32_RETUNE2_EXEC_TS_VALID |
                   NIOS_PKT_8x32_RETUNE2_EXEC_PLL_LOCKED);
        default:
            return 0;
    }
}

static int mock_bulk_transfer(void *driver, uint8_t endpoint, void *buffer,
                              uint32_t length, uint32_t timeout_ms,
                              uint32_t *transferred,
                              bool *transferred_valid)
{
    uint8_t *bytes = buffer;
    uint8_t target = 0, addr = 0;
    uint32_t data = 0;
    bool write = false;

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
        return 0;
    }
    if (endpoint != PERIPHERAL_EP_IN ||
        request[NIOS_PKT_8x32_IDX_MAGIC] != NIOS_PKT_8x32_MAGIC) {
        return BLADERF_ERR_UNEXPECTED;
    }

    nios_pkt_8x32_unpack(request, &target, &write, &addr, &data);
    if (target != NIOS_PKT_8x32_TARGET_RETUNE2_COMPLETION) {
        return BLADERF_ERR_UNEXPECTED;
    }
    if (write) {
        selected_addr = addr;
        selected_sequence = data;
    } else {
        if (selected_sequence != record_sequence ||
            selected_addr != NIOS_PKT_8x32_RETUNE2_COMPLETION_SELECT_RX) {
            return BLADERF_ERR_UNEXPECTED;
        }
        word_reads++;
        if (addr > highest_word) {
            highest_word = addr;
        }
        data = record_word(addr);
    }
    nios_pkt_8x32_resp_pack(bytes, target, write, addr, data, true);
    return 0;
}

static const struct usb_fns mock_usb_fns = {
    .bulk_transfer = mock_bulk_transfer,
};

static uint64_t mock_get_capabilities(struct bladerf *dev)
{
    (void)dev;
    return mock_capabilities;
}

static const struct board_fns mock_board = {
    .get_capabilities = mock_get_capabilities,
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

static int read_record(enum mock_mode mode, uint64_t capabilities,
                       int expected_status)
{
    struct bladerf_usb usb = { .fn = &mock_usb_fns, .driver = NULL };
    usb.peripheral = &usb.peripheral_local;
    struct bladerf dev = { .backend_data = &usb, .board = &mock_board };
    uint64_t frequency = 1, scheduled = 1, executed = 1;
    uint16_t profile = 1, token = 1;
    uint32_t session = 1;
    uint8_t flags = 0xff;
    int status;

    MUTEX_INIT(&usb.peripheral_local.lock);
    mock_mode = mode;
    mock_capabilities = capabilities;
    word_reads = 0;
    highest_word = 0;
    status = nios_retune2_completion_execution_read(
        &dev, BLADERF_MODULE_RX, record_sequence, &frequency, &scheduled,
        &profile, &session, &token, &executed, &flags);
    MUTEX_DESTROY(&usb.peripheral_local.lock);

    if (status != expected_status) {
        fprintf(stderr, "mode %d: status %d, expected %d\n", mode, status,
                expected_status);
        return 1;
    }
    if (expected_status != 0) {
        if (executed != 0 && status != BLADERF_ERR_UNSUPPORTED) {
            fprintf(stderr, "mode %d: failed read published executed=0x%llx\n",
                    mode, (unsigned long long)executed);
            return 1;
        }
        return 0;
    }
    if (frequency != UINT64_C(2400000000) ||
        scheduled != UINT64_C(0x1289abcdef) || profile != 9 ||
        session != 77 || token != 0x1234 ||
        executed != UINT64_C(0x1289abd000) ||
        flags != (NIOS_PKT_8x32_RETUNE2_EXEC_TS_VALID |
                  NIOS_PKT_8x32_RETUNE2_EXEC_PLL_LOCKED) ||
        highest_word != NIOS_PKT_8x32_RETUNE2_COMPLETION_EXEC_FLAGS ||
        word_reads != 12) {
        fprintf(stderr, "execution record mismatch: executed=0x%llx "
                        "flags=0x%x words=%u highest=%u\n",
                (unsigned long long)executed, flags, word_reads,
                highest_word);
        return 1;
    }
    return 0;
}

int main(void)
{
    const uint64_t caps = BLADERF_CAP_RETUNE2_REQUEST_TOKEN |
                          BLADERF_CAP_RETUNE2_EXECUTION_RECORD;

    if (read_record(MOCK_RECORD, caps, 0) ||
        read_record(MOCK_SEQUENCE_CHANGES, caps, BLADERF_ERR_WOULD_BLOCK) ||
        read_record(MOCK_UNKNOWN_FLAG, caps, BLADERF_ERR_UNEXPECTED) ||
        read_record(MOCK_RECORD, BLADERF_CAP_RETUNE2_REQUEST_TOKEN,
                    BLADERF_ERR_UNSUPPORTED)) {
        return 1;
    }
    puts("NIOS retune2 execution record host reads: PASS");
    return 0;
}
