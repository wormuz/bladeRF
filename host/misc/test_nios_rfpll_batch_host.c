#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "board/board.h"
#include "backend/usb/usb.h"
#include "backend/usb/nios_access.h"
#include "backend/usb/nios_transaction.h"
#include "nios_pkt_16x64.h"
#include "nios_pkt_8x32.h"
#include "nios_pkt_formats.h"
#include "nios_pkt_retune.h"
#include "nios_pkt_retune2.h"

enum response_mode {
    RESPONSE_SUCCESS,
    RESPONSE_OLD_FIRMWARE_ECHO,
    RESPONSE_PARTIAL_FAILURE,
    RESPONSE_UPDATE_SUCCESS,
    RESPONSE_UPDATE_OLD_FIRMWARE_ECHO,
    RESPONSE_UPDATE_FAILURE,
    RESPONSE_SNAPSHOT_SUCCESS,
    RESPONSE_SNAPSHOT_UNSUPPORTED,
    RESPONSE_SPI_READ_SUCCESS,
    RESPONSE_SPI_READ_FAILURE,
    RESPONSE_RFIC_READ_TIMEOUT,
    RESPONSE_8X32_READ_TIMEOUT,
    RESPONSE_SNAPSHOT_TIMEOUT,
    RESPONSE_WRONG_8X32_TARGET,
    RESPONSE_LEGACY_ERROR_MARKER,
    RESPONSE_RETUNE_CLEAR_PENDING_THEN_SUCCESS,
    RESPONSE_RETUNE2_CLEAR_PENDING_THEN_SUCCESS,
    RESPONSE_RETUNE2_FAILURE_STAGE,
    RESPONSE_RETUNE2_SUCCESS,
};

static enum response_mode response_mode;
static uint8_t request[NIOS_PKT_LEN];
static unsigned int out_count;
static unsigned int in_count;
static unsigned int timeout_on_in_count;
static unsigned int pending_replies_remaining;
static uint32_t last_in_timeout_ms;

static int mock_transfer_success(uint32_t length, uint32_t *transferred,
                                bool *transferred_valid)
{
    if (transferred != NULL) {
        *transferred = length;
    }
    if (transferred_valid != NULL) {
        *transferred_valid = true;
    }
    return 0;
}

static uint64_t mock_no_capabilities(struct bladerf *dev)
{
    (void)dev;
    return 0;
}

static const struct board_fns mock_board = {
    .get_capabilities = mock_no_capabilities,
};

static int check_8x32_packet_round_trip(void)
{
    uint8_t packed[NIOS_PKT_LEN];
    uint8_t target = 0, addr = 0;
    uint32_t data = 0;
    bool write = false;

    nios_pkt_8x32_pack(packed, NIOS_PKT_8x32_TARGET_RF_LINK_STATUS, true,
                       0xa5, UINT32_C(0xfedcba98));
    nios_pkt_8x32_unpack(packed, &target, &write, &addr, &data);
    return target == NIOS_PKT_8x32_TARGET_RF_LINK_STATUS && write &&
           addr == 0xa5 && data == UINT32_C(0xfedcba98) ? 0 : 1;
}

static int mock_bulk_transfer(void *driver, uint8_t endpoint, void *buffer,
                              uint32_t length, uint32_t timeout_ms,
                              uint32_t *transferred,
                              bool *transferred_valid)
{
    uint8_t *bytes = buffer;
    uint8_t target;
    uint8_t target8, addr8;
    bool write;
    bool write8;
    uint16_t count;
    uint64_t data;
    uint32_t data8;
    (void)driver;
    if (transferred != NULL) {
        *transferred = 0;
    }
    if (transferred_valid != NULL) {
        *transferred_valid = true;
    }
    if (endpoint == PERIPHERAL_EP_IN) {
        last_in_timeout_ms = timeout_ms;
    }

    if (length != NIOS_PKT_LEN) {
        return BLADERF_ERR_UNEXPECTED;
    }

    if (endpoint == PERIPHERAL_EP_OUT) {
        memcpy(request, bytes, length);
        out_count++;
        return mock_transfer_success(length, transferred,
                                     transferred_valid);
    }

    if (endpoint != PERIPHERAL_EP_IN) {
        return BLADERF_ERR_UNEXPECTED;
    }

    in_count++;
    if (timeout_on_in_count != 0 && in_count == timeout_on_in_count) {
        return BLADERF_ERR_TIMEOUT;
    }
    if (request[0] == NIOS_PKT_LEGACY_MAGIC &&
        response_mode == RESPONSE_LEGACY_ERROR_MARKER) {
        bytes[0] = NIOS_PKT_LEGACY_ERROR_MAGIC;
        return mock_transfer_success(length, transferred,
                                     transferred_valid);
    }
    if (request[0] == NIOS_PKT_RETUNE_MAGIC &&
        response_mode == RESPONSE_RETUNE_CLEAR_PENDING_THEN_SUCCESS) {
        const uint8_t flags = pending_replies_remaining != 0
            ? NIOS_PKT_RETUNERESP_FLAG_CANCEL_PENDING
            : NIOS_PKT_RETUNERESP_FLAG_SUCCESS;
        if (pending_replies_remaining != 0) {
            pending_replies_remaining--;
        }
        nios_pkt_retune_resp_pack(bytes, 0, 0, flags);
        return mock_transfer_success(length, transferred,
                                     transferred_valid);
    }
    if (request[0] == NIOS_PKT_RETUNE2_MAGIC &&
        response_mode == RESPONSE_RETUNE2_CLEAR_PENDING_THEN_SUCCESS) {
        const uint8_t flags = pending_replies_remaining != 0
            ? NIOS_PKT_RETUNE2_RESP_FLAG_CANCEL_PENDING
            : NIOS_PKT_RETUNE2_RESP_FLAG_SUCCESS;
        if (pending_replies_remaining != 0) {
            pending_replies_remaining--;
        }
        nios_pkt_retune2_resp_pack(bytes, 0, flags,
                                   NIOS_PKT_RETUNE2_FAILURE_NONE, 0, 0);
        return mock_transfer_success(length, transferred,
                                     transferred_valid);
    }
    if (request[0] == NIOS_PKT_RETUNE2_MAGIC &&
        response_mode == RESPONSE_RETUNE2_FAILURE_STAGE) {
        nios_pkt_retune2_resp_pack(
            bytes, 0, 0, NIOS_PKT_RETUNE2_FAILURE_PLL_LOCK_TIMEOUT, 0, 0);
        return mock_transfer_success(length, transferred,
                                     transferred_valid);
    }
    if (request[0] == NIOS_PKT_RETUNE2_MAGIC &&
        response_mode == RESPONSE_RETUNE2_SUCCESS) {
        nios_pkt_retune2_resp_pack(
            bytes, 0,
            NIOS_PKT_RETUNE2_RESP_FLAG_SUCCESS |
                NIOS_PKT_RETUNE2_RESP_FLAG_TSVTUNE_VALID,
            NIOS_PKT_RETUNE2_FAILURE_NONE, 0, 0);
        return mock_transfer_success(length, transferred,
                                     transferred_valid);
    }
    if (request[0] == NIOS_PKT_8x32_MAGIC) {
        nios_pkt_8x32_unpack(request, &target8, &write8, &addr8, &data8);
        if (response_mode == RESPONSE_8X32_READ_TIMEOUT) {
            return BLADERF_ERR_TIMEOUT;
        }
        if (response_mode == RESPONSE_WRONG_8X32_TARGET) {
            nios_pkt_8x32_resp_pack(bytes, target8 + 1, write8, addr8, 0,
                                    true);
            return mock_transfer_success(length, transferred,
                                         transferred_valid);
        }
        nios_pkt_8x32_resp_pack(bytes, target8, write8, addr8, 0, true);
        return mock_transfer_success(length, transferred,
                                     transferred_valid);
    }
    nios_pkt_16x64_unpack(request, &target, &write, &count, &data);
    if (target == NIOS_PKT_16x64_TARGET_RX_EPOCH_SNAPSHOT && !write &&
        response_mode == RESPONSE_SNAPSHOT_TIMEOUT) {
        return BLADERF_ERR_TIMEOUT;
    }
    if (target == NIOS_PKT_16x64_TARGET_RFIC && !write &&
        response_mode == RESPONSE_RFIC_READ_TIMEOUT) {
        return BLADERF_ERR_TIMEOUT;
    }
    if (target == NIOS_PKT_16x64_TARGET_RX_EPOCH_SNAPSHOT && !write) {
        if (response_mode == RESPONSE_SNAPSHOT_SUCCESS) {
            nios_pkt_16x64_resp_pack(bytes, target, false, count,
                                     UINT64_C(0x10203040a0b0c0d0), true);
        } else if (response_mode == RESPONSE_SNAPSHOT_UNSUPPORTED) {
            nios_pkt_16x64_resp_pack(bytes, target, false, count, 0, false);
        } else {
            return BLADERF_ERR_UNEXPECTED;
        }
        return mock_transfer_success(length, transferred,
                                     transferred_valid);
    }
    if (target == NIOS_PKT_16x64_TARGET_AD9361 && !write) {
        if (response_mode == RESPONSE_SPI_READ_SUCCESS) {
            nios_pkt_16x64_resp_pack(bytes, target, false, count,
                                     UINT64_C(0x0200000000000000), true);
        } else if (response_mode == RESPONSE_SPI_READ_FAILURE) {
            nios_pkt_16x64_resp_pack(bytes, target, false, count, 0, false);
        } else {
            return BLADERF_ERR_UNEXPECTED;
        }
        return mock_transfer_success(length, transferred,
                                     transferred_valid);
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

    return mock_transfer_success(length, transferred, transferred_valid);
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
    usb.peripheral = &usb.peripheral_local;
    struct bladerf dev = { .backend_data = &usb };
    const uint16_t regs[] = { 0x23b, 0x23c, 0x23d };
    const uint8_t values[] = { 0x11, 0x22, 0x33 };
    int status;

    MUTEX_INIT(&usb.peripheral_local.lock);
    response_mode = mode;
    status = nios_ad9361_spi_write_batch(&dev, regs, values, 3);
    if (status != expected_status) {
        fprintf(stderr, "status mismatch: mode=%d got=%d expected=%d\n",
                mode, status, expected_status);
        MUTEX_DESTROY(&usb.peripheral_local.lock);
        return 1;
    }

    MUTEX_DESTROY(&usb.peripheral_local.lock);
    return 0;
}

static int expect_update(enum response_mode mode, int expected_status)
{
    struct bladerf_usb usb = { .fn = &mock_usb_fns, .driver = NULL };
    usb.peripheral = &usb.peripheral_local;
    struct bladerf dev = { .backend_data = &usb };
    int status;

    MUTEX_INIT(&usb.peripheral_local.lock);
    response_mode = mode;
    status = nios_ad9361_spi_update_bits(&dev, 0x23b, 0x30, 0x10);
    if (status != expected_status) {
        fprintf(stderr, "RMW status mismatch: mode=%d got=%d expected=%d\n",
                mode, status, expected_status);
        MUTEX_DESTROY(&usb.peripheral_local.lock);
        return 1;
    }
    MUTEX_DESTROY(&usb.peripheral_local.lock);
    return 0;
}

static int expect_snapshot(enum response_mode mode, int expected_status)
{
    struct bladerf_usb usb = { .fn = &mock_usb_fns, .driver = NULL };
    usb.peripheral = &usb.peripheral_local;
    struct bladerf dev = { .backend_data = &usb };
    uint32_t status_word = 0, timestamp_lo = 0;
    int status;

    MUTEX_INIT(&usb.peripheral_local.lock);
    response_mode = mode;
    status = nios_rx_epoch_status_snapshot_read(&dev, &status_word,
                                                &timestamp_lo);
    if (status != expected_status ||
        (expected_status == 0 &&
         (status_word != 0x10203040 || timestamp_lo != 0xa0b0c0d0))) {
        fprintf(stderr, "snapshot mismatch: mode=%d status=%d "
                        "status_word=0x%08x timestamp=0x%08x\n",
                mode, status, status_word, timestamp_lo);
        MUTEX_DESTROY(&usb.peripheral_local.lock);
        return 1;
    }
    MUTEX_DESTROY(&usb.peripheral_local.lock);
    return 0;
}

static int expect_spi_read(enum response_mode mode, int expected_status)
{
    struct bladerf_usb usb = { .fn = &mock_usb_fns, .driver = NULL };
    usb.peripheral = &usb.peripheral_local;
    struct bladerf dev = { .backend_data = &usb };
    uint64_t data = UINT64_MAX;
    int status;

    MUTEX_INIT(&usb.peripheral_local.lock);
    response_mode = mode;
    status = nios_ad9361_spi_read(&dev, 0x247, &data);
    if (status != expected_status ||
        (expected_status == 0 && data != UINT64_C(0x0200000000000000)) ||
        (expected_status != 0 && data != 0)) {
        fprintf(stderr, "SPI read mapping mismatch: mode=%d status=%d "
                        "data=%016llx\n", mode, status,
                (unsigned long long)data);
        return 1;
    }
    MUTEX_DESTROY(&usb.peripheral_local.lock);
    return 0;
}

static int expect_retune_clear_pending(enum response_mode mode, bool retune2)
{
    struct bladerf_usb usb = { .fn = &mock_usb_fns, .driver = NULL };
    usb.peripheral = &usb.peripheral_local;
    struct bladerf dev = { .backend_data = &usb, .board = &mock_board };
    int status;

    MUTEX_INIT(&usb.peripheral_local.lock);
    response_mode = mode;
    pending_replies_remaining = 2;
    out_count = 0;
    in_count = 0;
    if (retune2) {
        status = nios_retune2(&dev, BLADERF_CHANNEL_RX(0),
                              NIOS_PKT_RETUNE2_CLEAR_QUEUE, 0, 0, 0, 0, 0);
    } else {
        status = nios_retune(&dev, BLADERF_CHANNEL_RX(0),
                             NIOS_PKT_RETUNE_CLEAR_QUEUE, 0, 0, 0, 0,
                             false, 0, false);
    }

    if (status != 0 || out_count != 3 || in_count != 3) {
        fprintf(stderr, "retune clear pending mapping mismatch: mode=%d "
                        "status=%d out=%u in=%u\n",
                mode, status, out_count, in_count);
        MUTEX_DESTROY(&usb.peripheral_local.lock);
        return 1;
    }
    MUTEX_DESTROY(&usb.peripheral_local.lock);
    return 0;
}

static int expect_retune2_failure_stage(void)
{
    struct bladerf_usb usb = { .fn = &mock_usb_fns, .driver = NULL };
    usb.peripheral = &usb.peripheral_local;
    struct bladerf dev = { .backend_data = &usb, .board = &mock_board };
    int status;

    MUTEX_INIT(&usb.peripheral_local.lock);
    response_mode = RESPONSE_RETUNE2_FAILURE_STAGE;
    status = nios_retune2(&dev, BLADERF_CHANNEL_RX(0), BLADERF_RETUNE_NOW,
                          1, 1, 0x83, 0, 0);
    if (status != BLADERF_ERR_UNEXPECTED || !dev.nios_retune_trace.valid ||
        dev.nios_retune_trace.status != 0 ||
        dev.nios_retune_trace.device_duration_ticks != 0 ||
        dev.nios_retune_trace.device_failure_stage !=
            NIOS_PKT_RETUNE2_FAILURE_PLL_LOCK_TIMEOUT) {
        fprintf(stderr, "RETUNE2 failure-stage mapping mismatch: status=%d "
                        "valid=%d transport=%d stage=%u\n",
                status, dev.nios_retune_trace.valid,
                dev.nios_retune_trace.status,
                dev.nios_retune_trace.device_failure_stage);
        MUTEX_DESTROY(&usb.peripheral_local.lock);
        return 1;
    }
    MUTEX_DESTROY(&usb.peripheral_local.lock);
    return 0;
}

static int expect_tokenless_now_retunes_use_extended_response_deadline(void)
{
    struct bladerf_usb usb = { .fn = &mock_usb_fns, .driver = NULL };
    usb.peripheral = &usb.peripheral_local;
    struct bladerf dev = { .backend_data = &usb, .board = &mock_board };
    int status;

    MUTEX_INIT(&usb.peripheral_local.lock);
    response_mode = RESPONSE_RETUNE2_SUCCESS;
    last_in_timeout_ms = 0;
    status = nios_retune2(&dev, BLADERF_CHANNEL_TX(0), NIOS_PKT_RETUNE2_NOW,
                          0, 1, 0x83, 0, 0);
    if (status != 0 || last_in_timeout_ms != 120000u) {
        fprintf(stderr, "tokenless TX RETUNE2 did not use its scoped response "
                        "deadline: status=%d timeout=%u\n",
                status, last_in_timeout_ms);
        MUTEX_DESTROY(&usb.peripheral_local.lock);
        return 1;
    }
    MUTEX_DESTROY(&usb.peripheral_local.lock);

    MUTEX_INIT(&usb.peripheral_local.lock);
    response_mode = RESPONSE_RETUNE2_SUCCESS;
    last_in_timeout_ms = 0;
    status = nios_retune2(&dev, BLADERF_CHANNEL_RX(0), NIOS_PKT_RETUNE2_NOW,
                          0, 1, 0x83, 0, 0);
    if (status != 0 || last_in_timeout_ms != 5000u) {
        fprintf(stderr, "tokenless RX RETUNE2 did not use its scoped response "
                        "deadline: status=%d timeout=%u\n",
                status, last_in_timeout_ms);
        MUTEX_DESTROY(&usb.peripheral_local.lock);
        return 1;
    }
    MUTEX_DESTROY(&usb.peripheral_local.lock);
    return 0;
}

static int expect_tokenless_now_unknown_generation_fails_closed(void)
{
    struct bladerf_usb usb = { .fn = &mock_usb_fns, .driver = NULL };
    usb.peripheral = &usb.peripheral_local;
    struct bladerf dev = { .backend_data = &usb, .board = &mock_board };
    unsigned int requests_before;
    int status;

    MUTEX_INIT(&usb.peripheral_local.lock);
    response_mode = RESPONSE_RETUNE2_SUCCESS;
    timeout_on_in_count = in_count + 1;
    status = nios_retune2(&dev, BLADERF_CHANNEL_RX(0), NIOS_PKT_RETUNE2_NOW,
                          0, 1, 0x83, 0, 0);
    timeout_on_in_count = 0;
    /* No correlated session generation is available to identify a late
     * tokenless reply. A timeout must therefore poison this handle and be
     * reported as an unexpected transport state. */
    if (status != BLADERF_ERR_UNEXPECTED ||
        !usb.peripheral_local.desynchronized) {
        fprintf(stderr, "tokenless RETUNE2 timeout without generation "
                        "was not fail-closed: "
                        "status=%d desynchronized=%u\n",
                status, usb.peripheral_local.desynchronized);
        MUTEX_DESTROY(&usb.peripheral_local.lock);
        return 1;
    }

    requests_before = out_count;
    status = nios_retune2(&dev, BLADERF_CHANNEL_RX(0), NIOS_PKT_RETUNE2_NOW,
                          0, 1, 0x83, 0, 0);
    if (status != BLADERF_ERR_UNEXPECTED || out_count != requests_before) {
        fprintf(stderr, "tokenless RETUNE2 retry escaped desync fence: "
                        "status=%d out=%u before=%u\n",
                status, out_count, requests_before);
        MUTEX_DESTROY(&usb.peripheral_local.lock);
        return 1;
    }

    MUTEX_DESTROY(&usb.peripheral_local.lock);
    return 0;
}

static int expect_rfic_read_timeout_clears_output(void)
{
    struct bladerf_usb usb = { .fn = &mock_usb_fns, .driver = NULL };
    usb.peripheral = &usb.peripheral_local;
    struct bladerf dev = { .backend_data = &usb };
    uint64_t data = UINT64_MAX;
    int status;

    MUTEX_INIT(&usb.peripheral_local.lock);
    response_mode = RESPONSE_RFIC_READ_TIMEOUT;
    status = nios_rfic_command_read(&dev, 0x0100, &data);
    if (status != BLADERF_ERR_TIMEOUT || data != 0) {
        fprintf(stderr, "RFIC timeout left stale output: status=%d data=%016llx\n",
                status, (unsigned long long)data);
        MUTEX_DESTROY(&usb.peripheral_local.lock);
        return 1;
    }
    MUTEX_DESTROY(&usb.peripheral_local.lock);
    return 0;
}

static int expect_link_status_timeout_clears_output(void)
{
    struct bladerf_usb usb = { .fn = &mock_usb_fns, .driver = NULL };
    usb.peripheral = &usb.peripheral_local;
    struct bladerf dev = { .backend_data = &usb };
    uint32_t status_word = UINT32_MAX;
    int status;

    MUTEX_INIT(&usb.peripheral_local.lock);
    response_mode = RESPONSE_8X32_READ_TIMEOUT;
    status = nios_rf_link_status_read(&dev, &status_word);
    if (status != BLADERF_ERR_TIMEOUT || status_word != 0) {
        fprintf(stderr, "RF-link status timeout left stale output: "
                        "status=%d data=%08x\n", status, status_word);
        MUTEX_DESTROY(&usb.peripheral_local.lock);
        return 1;
    }
    MUTEX_DESTROY(&usb.peripheral_local.lock);
    return 0;
}

static int expect_mismatched_response_poisoned_channel(void)
{
    struct bladerf_usb usb = { .fn = &mock_usb_fns, .driver = NULL };
    usb.peripheral = &usb.peripheral_local;
    struct bladerf dev = { .backend_data = &usb };
    uint32_t status_word = UINT32_MAX;
    unsigned int requests_before = out_count;
    int status;

    MUTEX_INIT(&usb.peripheral_local.lock);
    response_mode = RESPONSE_WRONG_8X32_TARGET;
    status = nios_rf_link_status_read(&dev, &status_word);
    if (status != BLADERF_ERR_UNEXPECTED || status_word != 0 ||
        !usb.peripheral_local.desynchronized) {
        fprintf(stderr, "mismatched NIOS response was accepted: status=%d "
                        "value=0x%08x desynchronized=%u\n", status,
                status_word, usb.peripheral_local.desynchronized);
        MUTEX_DESTROY(&usb.peripheral_local.lock);
        return 1;
    }

    status = nios_rf_link_status_read(&dev, &status_word);
    if (status != BLADERF_ERR_UNEXPECTED || out_count != requests_before + 1) {
        fprintf(stderr, "desynchronized NIOS channel sent another request: "
                        "status=%d out=%u before=%u\n", status, out_count,
                requests_before);
        MUTEX_DESTROY(&usb.peripheral_local.lock);
        return 1;
    }
    MUTEX_DESTROY(&usb.peripheral_local.lock);
    return 0;
}

static int expect_legacy_error_marker_is_completed_response(void)
{
    struct bladerf_usb usb = { .fn = &mock_usb_fns, .driver = NULL };
    usb.peripheral = &usb.peripheral_local;
    uint8_t packet[NIOS_PKT_LEN] = { NIOS_PKT_LEGACY_MAGIC };
    enum nios_transaction_stage stage;
    int status;

    MUTEX_INIT(&usb.peripheral_local.lock);
    response_mode = RESPONSE_LEGACY_ERROR_MARKER;
    status = nios_usb_transaction(&usb, packet, sizeof(packet),
                                  PERIPHERAL_TIMEOUT_MS, NULL, NULL, &stage);
    if (status != 0 || stage != NIOS_TRANSACTION_COMPLETE ||
        packet[0] != NIOS_PKT_LEGACY_ERROR_MAGIC ||
        usb.peripheral_local.desynchronized) {
        fprintf(stderr, "legacy error marker mishandled: status=%d stage=%d "
                        "magic=0x%02x desynchronized=%u\n", status, stage,
                packet[0], usb.peripheral_local.desynchronized);
        MUTEX_DESTROY(&usb.peripheral_local.lock);
        return 1;
    }
    MUTEX_DESTROY(&usb.peripheral_local.lock);
    return 0;
}

static int expect_epoch_snapshot_timeout_clears_outputs(void)
{
    struct bladerf_usb usb = { .fn = &mock_usb_fns, .driver = NULL };
    usb.peripheral = &usb.peripheral_local;
    struct bladerf dev = { .backend_data = &usb };
    uint32_t status_word = UINT32_MAX;
    uint32_t timestamp_lo = UINT32_MAX;
    int status;

    MUTEX_INIT(&usb.peripheral_local.lock);
    response_mode = RESPONSE_SNAPSHOT_TIMEOUT;
    status = nios_rx_epoch_status_snapshot_read(&dev, &status_word,
                                                 &timestamp_lo);
    if (status != BLADERF_ERR_TIMEOUT || status_word != 0 ||
        timestamp_lo != 0) {
        fprintf(stderr, "epoch snapshot timeout left stale outputs: "
                        "status=%d state=%08x timestamp=%08x\n", status,
                status_word, timestamp_lo);
        MUTEX_DESTROY(&usb.peripheral_local.lock);
        return 1;
    }
    MUTEX_DESTROY(&usb.peripheral_local.lock);
    return 0;
}

static int expect_partial_dwell_summary_timeout_is_atomic(void)
{
    struct bladerf_usb usb = { .fn = &mock_usb_fns, .driver = NULL };
    usb.peripheral = &usb.peripheral_local;
    struct bladerf dev = { .backend_data = &usb };
    uint32_t words[BLADERF_DWELL_WORD_COUNT];
    uint32_t generation = UINT32_MAX;
    unsigned int i;
    int status;

    for (i = 0; i < BLADERF_DWELL_WORD_COUNT; i++) {
        words[i] = UINT32_MAX;
    }
    MUTEX_INIT(&usb.peripheral_local.lock);
    response_mode = RESPONSE_SUCCESS;
    timeout_on_in_count = in_count + 5;
    status = nios_dwell_summary_read(&dev, words, &generation);
    timeout_on_in_count = 0;
    if (status != BLADERF_ERR_TIMEOUT || generation != 0) {
        fprintf(stderr, "partial dwell timeout published generation: "
                        "status=%d generation=%u\n", status, generation);
        MUTEX_DESTROY(&usb.peripheral_local.lock);
        return 1;
    }
    for (i = 0; i < BLADERF_DWELL_WORD_COUNT; i++) {
        if (words[i] != 0) {
            fprintf(stderr, "partial dwell timeout published word %u: %u\n",
                    i, words[i]);
            MUTEX_DESTROY(&usb.peripheral_local.lock);
            return 1;
        }
    }
    MUTEX_DESTROY(&usb.peripheral_local.lock);
    return 0;
}

int main(void)
{
    if (check_8x32_packet_round_trip() ||
        expect(RESPONSE_SUCCESS, 0) ||
        expect(RESPONSE_OLD_FIRMWARE_ECHO, BLADERF_ERR_UNSUPPORTED) ||
        expect(RESPONSE_PARTIAL_FAILURE, BLADERF_ERR_FPGA_OP) ||
        expect_update(RESPONSE_UPDATE_SUCCESS, 0) ||
        expect_update(RESPONSE_UPDATE_OLD_FIRMWARE_ECHO,
                      BLADERF_ERR_UNSUPPORTED) ||
        expect_update(RESPONSE_UPDATE_FAILURE, BLADERF_ERR_FPGA_OP) ||
        expect_snapshot(RESPONSE_SNAPSHOT_SUCCESS, 0) ||
        expect_snapshot(RESPONSE_SNAPSHOT_UNSUPPORTED,
                        BLADERF_ERR_UNSUPPORTED) ||
        expect_spi_read(RESPONSE_SPI_READ_SUCCESS, 0) ||
        expect_spi_read(RESPONSE_SPI_READ_FAILURE, BLADERF_ERR_FPGA_OP) ||
        expect_retune_clear_pending(
            RESPONSE_RETUNE_CLEAR_PENDING_THEN_SUCCESS, false) ||
        expect_retune_clear_pending(
            RESPONSE_RETUNE2_CLEAR_PENDING_THEN_SUCCESS, true) ||
        expect_retune2_failure_stage() ||
        expect_tokenless_now_retunes_use_extended_response_deadline() ||
        expect_tokenless_now_unknown_generation_fails_closed() ||
        expect_rfic_read_timeout_clears_output() ||
        expect_link_status_timeout_clears_output() ||
        expect_mismatched_response_poisoned_channel() ||
        expect_legacy_error_marker_is_completed_response() ||
        expect_epoch_snapshot_timeout_clears_outputs() ||
        expect_partial_dwell_summary_timeout_is_atomic() ||
        out_count != 17 || in_count != 17) {
        if (out_count != 17 || in_count != 17) {
            fprintf(stderr, "NIOS transaction count mismatch: out=%u in=%u\n",
                    out_count, in_count);
        }
        return 1;
    }

    puts("libbladeRF RFPLL batch and atomic RMW mappings: PASS");
    return 0;
}
