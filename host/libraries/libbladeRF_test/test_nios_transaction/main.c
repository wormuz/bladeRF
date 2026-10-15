#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "backend/usb/nios_transaction.h"
#include "backend/usb/nios_deadline_policy.h"
#include "libbladeRF.h"
#include "nios_pkt_formats.h"

struct fake_usb {
    int calls;
    int fail_call;
    uint32_t timeout_transferred;
    bool timeout_length_unknown;
    uint8_t endpoints[16];
    uint32_t timeouts[16];
    uint8_t scripted_responses[4][NIOS_PKT_LEN];
    size_t scripted_response_count;
    size_t scripted_response_index;
    uint32_t scripted_response_offset;
    uint32_t scripted_first_chunk_size;
    bool make_register_response;
    bool mutate_response_target;
    bool mutate_response_data;
    bool mutate_response_reserved;
    uint8_t mutate_response_reserved_offset;
    bool mutate_response_flags;
    uint8_t mutate_response_flags_offset;
    bool make_legacy_config_response;
    bool make_legacy_error_response;
    bool mutate_legacy_config_response_addr;
    bool make_semantic_retune_response;
    bool make_semantic_retune2_response;
    uint8_t requests[16][NIOS_PKT_LEN];
};

static int fake_bulk_transfer(void *driver, uint8_t endpoint, void *buffer,
                              uint32_t length, uint32_t timeout_ms,
                              uint32_t *transferred,
                              bool *transferred_valid)
{
    struct fake_usb *fake = driver;
    int call = fake->calls++;
    (void)buffer;
    (void)length;

    assert(call < 16);
    fake->endpoints[call] = endpoint;
    fake->timeouts[call] = timeout_ms;
    if (endpoint == PERIPHERAL_EP_OUT && length == NIOS_PKT_LEN) {
        memcpy(fake->requests[call], buffer, NIOS_PKT_LEN);
    }
    if (transferred_valid != NULL) {
        *transferred_valid = !(endpoint == PERIPHERAL_EP_IN &&
                               call + 1 == fake->fail_call &&
                               fake->timeout_length_unknown);
    }
    if (endpoint == PERIPHERAL_EP_IN &&
        fake->scripted_response_index < fake->scripted_response_count) {
        uint32_t count = NIOS_PKT_LEN - fake->scripted_response_offset;
        if (count > length) {
            count = length;
        }
        if (fake->scripted_response_offset == 0 &&
            fake->scripted_first_chunk_size != 0 &&
            count > fake->scripted_first_chunk_size) {
            count = fake->scripted_first_chunk_size;
        }
        memcpy(buffer,
               fake->scripted_responses[fake->scripted_response_index] +
                   fake->scripted_response_offset,
               count);
        fake->scripted_response_offset += count;
        if (fake->scripted_response_offset == NIOS_PKT_LEN) {
            fake->scripted_response_index++;
            fake->scripted_response_offset = 0;
        }
        if (transferred != NULL) {
            *transferred = count;
        }
        return call + 1 == fake->fail_call ? BLADERF_ERR_TIMEOUT : 0;
    }
    if (transferred != NULL) {
        *transferred = endpoint == PERIPHERAL_EP_IN &&
                               call + 1 == fake->fail_call
                           ? fake->timeout_transferred : length;
    }
    if (endpoint == PERIPHERAL_EP_IN && fake->make_semantic_retune_response) {
        uint8_t *response = buffer;
        response[0] = NIOS_PKT_RETUNE_MAGIC;
        memset(response + NIOS_PKT_RETUNERESP_IDX_TIME, 0, 8);
        response[NIOS_PKT_RETUNERESP_IDX_VCOCAP] = 0;
        response[NIOS_PKT_RETUNERESP_IDX_FLAGS] =
            NIOS_PKT_RETUNERESP_FLAG_TSVTUNE_VALID |
            NIOS_PKT_RETUNERESP_FLAG_SUCCESS;
        memset(response + NIOS_PKT_RETUNERESP_IDX_RESV, 0,
               NIOS_PKT_LEN - NIOS_PKT_RETUNERESP_IDX_RESV);
    }
    if (endpoint == PERIPHERAL_EP_IN && fake->make_semantic_retune2_response) {
        uint8_t *response = buffer;
        response[0] = NIOS_PKT_RETUNE2_MAGIC;
        memset(response + NIOS_PKT_RETUNE2_RESP_IDX_TIME, 0, 8);
        response[NIOS_PKT_RETUNE2_RESP_IDX_FLAGS] =
            NIOS_PKT_RETUNE2_RESP_FLAG_TSVTUNE_VALID |
            NIOS_PKT_RETUNE2_RESP_FLAG_SUCCESS;
        memset(response + NIOS_PKT_RETUNE2_RESP_IDX_RESV, 0,
               NIOS_PKT_LEN - NIOS_PKT_RETUNE2_RESP_IDX_RESV);
    }
    if (endpoint == PERIPHERAL_EP_IN && fake->make_legacy_config_response) {
        uint8_t *response = buffer;
        response[2] = 0; /* PIO address normalized to its config base. */
        response[3] = 0x5a;
        if (fake->mutate_legacy_config_response_addr) {
            response[2] = 1;
        }
    }
    if (endpoint == PERIPHERAL_EP_IN && fake->make_legacy_error_response) {
        ((uint8_t *)buffer)[0] = NIOS_PKT_LEGACY_ERROR_MAGIC;
    }
    if (endpoint == PERIPHERAL_EP_IN && fake->make_register_response) {
        uint8_t target;
        bool is_write;

        nios_pkt_16x64_unpack(buffer, &target, &is_write, NULL, NULL);
        uint8_t *response = buffer;
        response[NIOS_PKT_16x64_IDX_FLAGS] |=
            NIOS_PKT_16x64_FLAG_SUCCESS;
        /* Reads return data different from the request. Identity validation
         * must compare only the transaction key, not response payload.
         * Standard RFIC write ACKs echo the payload, so preserve it there. */
        if (!is_write || target != NIOS_PKT_16x64_TARGET_RFIC) {
            response[NIOS_PKT_16x64_IDX_DATA] = 0x5a;
        }
    }
    if (endpoint == PERIPHERAL_EP_IN && fake->mutate_response_target) {
        ((uint8_t *)buffer)[NIOS_PKT_16x64_IDX_TARGET_ID] ^= 1u;
    }
    if (endpoint == PERIPHERAL_EP_IN && fake->mutate_response_data) {
        ((uint8_t *)buffer)[NIOS_PKT_16x64_IDX_DATA] ^= 1u;
    }
    if (endpoint == PERIPHERAL_EP_IN && fake->mutate_response_reserved) {
        ((uint8_t *)buffer)[fake->mutate_response_reserved_offset] = 1u;
    }
    if (endpoint == PERIPHERAL_EP_IN && fake->mutate_response_flags) {
        ((uint8_t *)buffer)[fake->mutate_response_flags_offset] |= 0x80u;
    }
    return call + 1 == fake->fail_call ? BLADERF_ERR_TIMEOUT : 0;
}

static void setup(struct bladerf_usb *usb, struct usb_fns *fns,
                  struct fake_usb *fake)
{
    memset(usb, 0, sizeof(*usb));
    memset(fns, 0, sizeof(*fns));
    memset(fake, 0, sizeof(*fake));
    usb->peripheral = &usb->peripheral_local;
    fns->bulk_transfer = fake_bulk_transfer;
    usb->fn = fns;
    usb->driver = fake;
    assert(MUTEX_INIT(&usb->peripheral->lock) == 0);
}

static void teardown(struct bladerf_usb *usb)
{
    MUTEX_DESTROY(&usb->peripheral->lock);
}

static void test_success(void)
{
    struct bladerf_usb usb;
    struct usb_fns fns;
    struct fake_usb fake;
    enum nios_transaction_stage stage = NIOS_TRANSACTION_DESYNCHRONIZED;
    uint8_t buffer[16] = { 0 };

    _Static_assert(NIOS_RFIC_RESPONSE_TIMEOUT_MS == 1000u,
                   "NIOS acceptance/status response must fail promptly");

    setup(&usb, &fns, &fake);
    assert(nios_usb_transaction(&usb, buffer, sizeof(buffer),
                                NIOS_RFIC_RESPONSE_TIMEOUT_MS, NULL, NULL,
                                &stage) == 0);
    assert(stage == NIOS_TRANSACTION_COMPLETE);
    assert(fake.calls == 2);
    assert(fake.endpoints[0] == PERIPHERAL_EP_OUT);
    assert(fake.endpoints[1] == PERIPHERAL_EP_IN);
    assert(fake.timeouts[0] == PERIPHERAL_TIMEOUT_MS);
    assert(fake.timeouts[1] == NIOS_RFIC_RESPONSE_TIMEOUT_MS);
    assert(!usb.peripheral->desynchronized);
    teardown(&usb);
}

static void test_in_timeout_fails_closed(void)
{
    struct bladerf_usb usb;
    struct usb_fns fns;
    struct fake_usb fake;
    enum nios_transaction_stage stage;
    uint8_t buffer[16] = { 0 };

    setup(&usb, &fns, &fake);
    fake.fail_call = 2;
    assert(nios_usb_transaction(&usb, buffer, sizeof(buffer),
                                NIOS_RFIC_RESPONSE_TIMEOUT_MS, NULL, NULL,
                                &stage) == BLADERF_ERR_TIMEOUT);
    assert(stage == NIOS_TRANSACTION_IN_FAILED);
    assert(fake.calls == 2);
    assert(usb.peripheral->desynchronized);

    assert(nios_usb_transaction(&usb, buffer, sizeof(buffer),
                                NIOS_RFIC_RESPONSE_TIMEOUT_MS, NULL, NULL,
                                &stage) == BLADERF_ERR_UNEXPECTED);
    assert(stage == NIOS_TRANSACTION_DESYNCHRONIZED);
    assert(fake.calls == 2);
    teardown(&usb);
}

static void test_zero_byte_read_timeout_recovers_through_ordered_barrier(void)
{
    struct bladerf_usb usb;
    struct usb_fns fns;
    struct fake_usb fake;
    enum nios_transaction_stage stage;
    uint8_t request[NIOS_PKT_LEN] = {0};

    setup(&usb, &fns, &fake);
    nios_pkt_8x32_pack(request, NIOS_PKT_8x32_TARGET_VERSION, false, 0, 0);
    fake.fail_call = 2;
    fake.timeout_transferred = 0;
    assert(nios_usb_transaction(&usb, request, sizeof(request), 20,
                                NULL, NULL, &stage) == BLADERF_ERR_TIMEOUT);
    assert(stage == NIOS_TRANSACTION_IN_FAILED);
    assert(usb.peripheral->pending_read_response);
    assert(!usb.peripheral->desynchronized);

    /* The old read response arrives late, followed by the distinct barrier. */
    fake.fail_call = 0;
    fake.scripted_response_count = 2;
    nios_pkt_8x32_resp_pack(fake.scripted_responses[0],
                            NIOS_PKT_8x32_TARGET_VERSION, false, 0,
                            0x01020304, true);
    nios_pkt_8x32_resp_pack(fake.scripted_responses[1],
                            NIOS_PKT_8x32_TARGET_CONTROL, false, 0,
                            0x01020304, true);
    nios_pkt_8x32_pack(request, NIOS_PKT_8x32_TARGET_VERSION, false, 2, 0);
    assert(nios_usb_transaction(&usb, request, sizeof(request), 100,
                                NULL, NULL, &stage) == 0);
    assert(stage == NIOS_TRANSACTION_COMPLETE);
    assert(fake.requests[2][NIOS_PKT_8x32_IDX_MAGIC] == NIOS_PKT_8x32_MAGIC);
    assert(fake.requests[2][NIOS_PKT_8x32_IDX_TARGET_ID] ==
           NIOS_PKT_8x32_TARGET_CONTROL);
    assert(fake.requests[2][NIOS_PKT_8x32_IDX_ADDR] == 0);
    assert(!usb.peripheral->pending_read_response);
    assert(!usb.peripheral->desynchronized);
    teardown(&usb);
}

static void test_zero_byte_read_timeout_recovers_when_original_was_lost(void)
{
    struct bladerf_usb usb;
    struct usb_fns fns;
    struct fake_usb fake;
    enum nios_transaction_stage stage;
    uint8_t request[NIOS_PKT_LEN] = {0};

    setup(&usb, &fns, &fake);
    nios_pkt_16x64_pack(request, NIOS_PKT_16x64_TARGET_RFIC, false,
                        0x0f00, 0);
    fake.fail_call = 2;
    fake.timeout_transferred = 0;
    assert(nios_usb_transaction(&usb, request, sizeof(request), 20,
                                NULL, NULL, &stage) == BLADERF_ERR_TIMEOUT);
    assert(usb.peripheral->pending_read_response);
    assert(!usb.peripheral->desynchronized);

    /* If the original was lost, the first ordered response is the barrier. */
    fake.fail_call = 0;
    fake.scripted_response_count = 1;
    nios_pkt_8x32_resp_pack(fake.scripted_responses[0],
                            NIOS_PKT_8x32_TARGET_VERSION, false, 0,
                            0x01020304, true);
    nios_pkt_16x64_pack(request, NIOS_PKT_16x64_TARGET_RFIC, false,
                        0x0f0c, 0);
    assert(nios_usb_transaction(&usb, request, sizeof(request), 100,
                                NULL, NULL, &stage) == 0);
    assert(stage == NIOS_TRANSACTION_COMPLETE);
    assert(!usb.peripheral->pending_read_response);
    assert(!usb.peripheral->desynchronized);
    teardown(&usb);
}

static void test_ordered_barrier_timeout_remains_fail_closed(void)
{
    struct bladerf_usb usb;
    struct usb_fns fns;
    struct fake_usb fake;
    enum nios_transaction_stage stage;
    uint8_t request[NIOS_PKT_LEN] = {0};

    setup(&usb, &fns, &fake);
    nios_pkt_8x32_pack(request, NIOS_PKT_8x32_TARGET_VERSION, false, 0, 0);
    fake.fail_call = 2;
    fake.timeout_transferred = 0;
    assert(nios_usb_transaction(&usb, request, sizeof(request), 20,
                                NULL, NULL, &stage) == BLADERF_ERR_TIMEOUT);
    assert(usb.peripheral->pending_read_response);

    fake.fail_call = 4; /* barrier IN times out with no bytes */
    assert(nios_usb_transaction(&usb, request, sizeof(request), 20,
                                NULL, NULL, &stage) == BLADERF_ERR_TIMEOUT);
    assert(stage == NIOS_TRANSACTION_IN_FAILED);
    assert(usb.peripheral->pending_read_response);
    assert(usb.peripheral->desynchronized);
    assert(fake.calls == 4); /* never send the caller's request */
    teardown(&usb);
}

static void test_read_barrier_drains_pending_rfic_duplicate_acks(void)
{
    struct bladerf_usb usb;
    struct usb_fns fns;
    struct fake_usb fake;
    enum nios_transaction_stage stage;
    uint8_t request[NIOS_PKT_LEN] = {0};

    setup(&usb, &fns, &fake);
    nios_pkt_16x64_pack_with_transaction_id(
        request, NIOS_PKT_16x64_TARGET_RFIC, true, 0x0104, 0x12345678,
        0x5a31);
    fake.fail_call = 2;
    assert(nios_usb_transaction(&usb, request, sizeof(request), 20,
                                NULL, NULL, &stage) == BLADERF_ERR_TIMEOUT);
    assert(usb.peripheral->pending_rfic_write);

    /* A recovery status read also times out before any bytes. Both the read
     * response and duplicate write ACKs may now precede the next barrier. */
    nios_pkt_16x64_pack(request, NIOS_PKT_16x64_TARGET_RFIC, false, 0x0f0c,
                        0);
    fake.fail_call = 4;
    assert(nios_usb_transaction(&usb, request, sizeof(request), 20,
                                NULL, NULL, &stage) == BLADERF_ERR_TIMEOUT);
    assert(usb.peripheral->pending_read_response);
    assert(usb.peripheral->pending_rfic_write);
    assert(!usb.peripheral->desynchronized);

    fake.fail_call = 0;
    nios_pkt_16x64_resp_pack_with_transaction_id(
        fake.scripted_responses[0], NIOS_PKT_16x64_TARGET_RFIC, true,
        0x0104, 0x12345678, true, 0x5a31);
    nios_pkt_16x64_resp_pack_with_transaction_id(
        fake.scripted_responses[1], NIOS_PKT_16x64_TARGET_RFIC, true,
        0x0104, 0x12345678, true, 0x5a31);
    nios_pkt_8x32_resp_pack(fake.scripted_responses[2],
                            NIOS_PKT_8x32_TARGET_VERSION, false, 0,
                            0x01020304, true);
    nios_pkt_16x64_resp_pack(fake.scripted_responses[3],
                             NIOS_PKT_16x64_TARGET_RFIC, false, 0x0f00,
                             0, true);
    fake.scripted_response_count = 4;

    nios_pkt_16x64_pack(request, NIOS_PKT_16x64_TARGET_RFIC, false, 0x0f00,
                        0);
    assert(nios_usb_transaction(&usb, request, sizeof(request), 100,
                                NULL, NULL, &stage) == 0);
    assert(stage == NIOS_TRANSACTION_COMPLETE);
    assert(fake.scripted_response_index == 4);
    assert(!usb.peripheral->pending_read_response);
    assert(!usb.peripheral->pending_rfic_write);
    assert(!usb.peripheral->desynchronized);
    teardown(&usb);
}

static void test_tagged_write_full_ack_with_timeout_status_is_consumed(void)
{
    struct bladerf_usb usb;
    struct usb_fns fns;
    struct fake_usb fake;
    enum nios_transaction_stage stage;
    uint8_t buffer[NIOS_PKT_LEN] = {0};

    setup(&usb, &fns, &fake);
    fake.fail_call = 2;
    fake.timeout_transferred = NIOS_PKT_LEN;
    fake.make_register_response = true;
    nios_pkt_16x64_pack_with_transaction_id(
        buffer, NIOS_PKT_16x64_TARGET_RFIC, true, 0x0104, 0x1234, 0x5a31);
    assert(nios_usb_transaction(&usb, buffer, sizeof(buffer), 250, NULL,
                                NULL, &stage) == 0);
    assert(stage == NIOS_TRANSACTION_COMPLETE);
    assert(!usb.peripheral->pending_rfic_write);
    assert(!usb.peripheral->desynchronized);
    teardown(&usb);
}

static void test_tagged_rfic_busy_response_is_a_valid_terminal_reply(void)
{
    struct bladerf_usb usb;
    struct usb_fns fns;
    struct fake_usb fake;
    enum nios_transaction_stage stage = NIOS_TRANSACTION_DESYNCHRONIZED;
    uint8_t request[NIOS_PKT_LEN] = {0};

    setup(&usb, &fns, &fake);
    nios_pkt_16x64_pack_with_transaction_id(
        request, NIOS_PKT_16x64_TARGET_RFIC, true, 0x0104, 0x1234, 0x5a31);
    nios_pkt_16x64_resp_pack_busy(
        fake.scripted_responses[0], NIOS_PKT_16x64_TARGET_RFIC, 0x0104,
        0x1234, 0x5a31);
    fake.scripted_response_count = 1;

    assert(nios_usb_transaction(&usb, request, sizeof(request), 250, NULL,
                                NULL, &stage) == 0);
    assert(stage == NIOS_TRANSACTION_COMPLETE);
    assert(nios_pkt_16x64_resp_is_busy(fake.scripted_responses[0]));
    assert((fake.scripted_responses[0][NIOS_PKT_16x64_IDX_FLAGS] &
            NIOS_PKT_16x64_FLAG_SUCCESS) == 0);
    assert(!usb.peripheral->pending_rfic_write);
    assert(!usb.peripheral->desynchronized);
    teardown(&usb);
}

static void test_retune_busy_responses_are_terminal_and_correlated(void)
{
    struct bladerf_usb usb;
    struct usb_fns fns;
    struct fake_usb fake;
    enum nios_transaction_stage stage = NIOS_TRANSACTION_DESYNCHRONIZED;
    uint8_t request[NIOS_PKT_LEN] = {0};

    setup(&usb, &fns, &fake);
    nios_pkt_retune_pack(request, BLADERF_MODULE_RX, NIOS_PKT_RETUNE_NOW,
                         0, 0, 0, 0, false, 0, false);
    nios_pkt_retune_resp_pack(fake.scripted_responses[0], 0, 0,
                              NIOS_PKT_RETUNERESP_FLAG_BUSY);
    fake.scripted_response_count = 1;
    assert(nios_usb_transaction(&usb, request, sizeof(request), 250, NULL,
                                NULL, &stage) == 0);
    assert(stage == NIOS_TRANSACTION_COMPLETE);
    assert(!usb.peripheral->desynchronized);
    teardown(&usb);

    setup(&usb, &fns, &fake);
    nios_pkt_retune2_pack(request, BLADERF_MODULE_RX, NIOS_PKT_RETUNE2_NOW,
                          0, 0, 0x83, 0x00);
    nios_pkt_retune2_resp_pack(
        fake.scripted_responses[0], 0, NIOS_PKT_RETUNE2_RESP_FLAG_BUSY,
        NIOS_PKT_RETUNE2_FAILURE_NONE, 0, 0);
    fake.scripted_response_count = 1;
    assert(nios_usb_transaction(&usb, request, sizeof(request), 250, NULL,
                                NULL, &stage) == 0);
    assert(stage == NIOS_TRANSACTION_COMPLETE);
    assert(!usb.peripheral->desynchronized);
    teardown(&usb);
}

static void test_retune2_new_failure_stages_round_trip(void)
{
    uint8_t response[NIOS_PKT_LEN] = {0};

    nios_pkt_retune2_resp_pack(
        response, 0, 0, NIOS_PKT_RETUNE2_FAILURE_TX_QUAD_CALIBRATION, 0, 0);
    assert(nios_pkt_retune2_resp_failure(response) ==
           NIOS_PKT_RETUNE2_FAILURE_TX_QUAD_CALIBRATION);

    nios_pkt_retune2_resp_pack(
        response, 0, 0, NIOS_PKT_RETUNE2_FAILURE_RFIC_FAULTED, 0, 0);
    assert(nios_pkt_retune2_resp_failure(response) ==
           NIOS_PKT_RETUNE2_FAILURE_RFIC_FAULTED);

    nios_pkt_retune2_resp_pack(response, 0, 0, UINT8_MAX, 0, 0);
    assert(nios_pkt_retune2_resp_failure(response) ==
           NIOS_PKT_RETUNE2_FAILURE_NONE);
}

static void test_retune2_response_echoes_request_token(void)
{
    struct bladerf_usb usb;
    struct usb_fns fns;
    struct fake_usb fake;
    enum nios_transaction_stage stage;
    uint8_t request[NIOS_PKT_LEN];

    setup(&usb, &fns, &fake);
    nios_pkt_8x32_pack(request,
                       NIOS_PKT_8x32_TARGET_COMMAND_SESSION_GENERATION,
                       false, 0, 0);
    nios_pkt_8x32_resp_pack(fake.scripted_responses[0],
                            NIOS_PKT_8x32_TARGET_COMMAND_SESSION_GENERATION,
                            false, 0, 0xabcdef01, true);
    fake.scripted_response_count = 1;
    assert(nios_usb_transaction(&usb, request, sizeof(request), 250, NULL,
                                NULL, &stage) == 0);
    assert(usb.peripheral->command_session_generation_valid);
    assert(usb.peripheral->command_session_generation == 0xabcdef01);

    nios_pkt_retune2_pack_token(request, BLADERF_MODULE_RX,
                                UINT64_C(0x12345678), 1, 0, 0x83, 0, 0x1234);
    nios_pkt_retune2_resp_pack(
        fake.scripted_responses[1], 0,
        NIOS_PKT_RETUNE2_RESP_FLAG_SUCCESS,
        NIOS_PKT_RETUNE2_FAILURE_NONE, 0x4321, 0xabcdef01);
    fake.scripted_response_count = 2;
    assert(nios_usb_transaction(&usb, request, sizeof(request), 250, NULL,
                                NULL, &stage) ==
           BLADERF_ERR_UNEXPECTED);
    assert(stage == NIOS_TRANSACTION_RESPONSE_MISMATCH);
    assert(usb.peripheral->desynchronized);
    teardown(&usb);

    setup(&usb, &fns, &fake);
    nios_pkt_8x32_pack(request,
                       NIOS_PKT_8x32_TARGET_COMMAND_SESSION_GENERATION,
                       false, 0, 0);
    nios_pkt_8x32_resp_pack(fake.scripted_responses[0],
                            NIOS_PKT_8x32_TARGET_COMMAND_SESSION_GENERATION,
                            false, 0, 0xabcdef01, true);
    fake.scripted_response_count = 1;
    assert(nios_usb_transaction(&usb, request, sizeof(request), 250, NULL,
                                NULL, &stage) == 0);

    nios_pkt_retune2_pack_token(request, BLADERF_MODULE_RX,
                                UINT64_C(0x12345678), 1, 0, 0x83, 0, 0x1234);
    nios_pkt_retune2_resp_pack(
        fake.scripted_responses[1], 0,
        NIOS_PKT_RETUNE2_RESP_FLAG_SUCCESS,
        NIOS_PKT_RETUNE2_FAILURE_NONE, 0x1234, 0xabcdef02);
    fake.scripted_response_count = 2;
    assert(nios_usb_transaction(&usb, request, sizeof(request), 250, NULL,
                                NULL, &stage) == BLADERF_ERR_UNEXPECTED);
    assert(stage == NIOS_TRANSACTION_RESPONSE_MISMATCH);
    assert(usb.peripheral->desynchronized);
    teardown(&usb);

    setup(&usb, &fns, &fake);
    nios_pkt_8x32_pack(request,
                       NIOS_PKT_8x32_TARGET_COMMAND_SESSION_GENERATION,
                       false, 0, 0);
    nios_pkt_8x32_resp_pack(fake.scripted_responses[0],
                            NIOS_PKT_8x32_TARGET_COMMAND_SESSION_GENERATION,
                            false, 0, 0xabcdef01, true);
    fake.scripted_response_count = 1;
    assert(nios_usb_transaction(&usb, request, sizeof(request), 250, NULL,
                                NULL, &stage) == 0);

    nios_pkt_retune2_pack_token(request, BLADERF_MODULE_RX,
                                UINT64_C(0x12345678), 1, 0, 0x83, 0, 0x1234);
    nios_pkt_retune2_resp_pack(
        fake.scripted_responses[1], 0,
        NIOS_PKT_RETUNE2_RESP_FLAG_SUCCESS,
        NIOS_PKT_RETUNE2_FAILURE_NONE, 0x1234, 0xabcdef01);
    fake.scripted_response_count = 2;
    assert(nios_usb_transaction(&usb, request, sizeof(request), 250, NULL,
                                NULL, &stage) == 0);
    assert(stage == NIOS_TRANSACTION_COMPLETE);
    teardown(&usb);
}

static void test_tokened_now_async_acceptance_response(void)
{
    struct bladerf_usb usb;
    struct usb_fns fns;
    struct fake_usb fake;
    enum nios_transaction_stage stage;
    uint8_t request[NIOS_PKT_LEN];
    uint32_t const session_generation = 0xabcdef01;
    uint16_t const request_token = 0x1234;

    setup(&usb, &fns, &fake);
    nios_pkt_8x32_pack(request,
                       NIOS_PKT_8x32_TARGET_COMMAND_SESSION_GENERATION,
                       false, 0, 0);
    nios_pkt_8x32_resp_pack(fake.scripted_responses[0],
                            NIOS_PKT_8x32_TARGET_COMMAND_SESSION_GENERATION,
                            false, 0, session_generation, true);
    fake.scripted_response_count = 1;
    assert(nios_usb_transaction(&usb, request, sizeof(request), 250, NULL,
                                NULL, &stage) == 0);
    assert(usb.peripheral->command_session_generation_valid);
    assert(usb.peripheral->command_session_generation == session_generation);

    nios_pkt_retune2_pack_token(request, BLADERF_MODULE_RX,
                                NIOS_PKT_RETUNE2_NOW, 1, 0, 0x83, 0,
                                request_token);
    nios_pkt_retune2_resp_pack(
        fake.scripted_responses[1], 0,
        NIOS_PKT_RETUNE2_RESP_FLAG_SUCCESS |
            NIOS_PKT_RETUNE2_RESP_FLAG_ASYNC_PENDING,
        NIOS_PKT_RETUNE2_FAILURE_NONE, request_token,
        session_generation);
    nios_pkt_retune2_resp_set_async_baseline(
        fake.scripted_responses[1], 0x10203040, 0x50607080);
    fake.scripted_response_count = 2;

    assert(nios_usb_transaction(&usb, request, sizeof(request), 250, NULL,
                                NULL, &stage) == 0);
    assert(stage == NIOS_TRANSACTION_COMPLETE);
    {
        uint32_t success_baseline;
        uint32_t failure_baseline;
        nios_pkt_retune2_resp_get_async_baseline(
            fake.scripted_responses[1], &success_baseline, &failure_baseline);
        assert(success_baseline == 0x10203040);
        assert(failure_baseline == 0x50607080);
    }
    teardown(&usb);

    setup(&usb, &fns, &fake);
    nios_pkt_8x32_pack(request,
                       NIOS_PKT_8x32_TARGET_COMMAND_SESSION_GENERATION,
                       false, 0, 0);
    nios_pkt_8x32_resp_pack(fake.scripted_responses[0],
                            NIOS_PKT_8x32_TARGET_COMMAND_SESSION_GENERATION,
                            false, 0, session_generation, true);
    fake.scripted_response_count = 1;
    assert(nios_usb_transaction(&usb, request, sizeof(request), 250, NULL,
                                NULL, &stage) == 0);
    nios_pkt_retune2_pack_token(request, BLADERF_MODULE_RX,
                                NIOS_PKT_RETUNE2_NOW, 1, 0, 0x83, 0,
                                request_token);
    nios_pkt_retune2_resp_pack(
        fake.scripted_responses[1], 0,
        NIOS_PKT_RETUNE2_RESP_FLAG_SUCCESS |
            NIOS_PKT_RETUNE2_RESP_FLAG_ASYNC_PENDING,
        NIOS_PKT_RETUNE2_FAILURE_NONE, request_token,
        session_generation);
    fake.scripted_response_count = 2;
    assert(nios_usb_transaction(&usb, request, sizeof(request), 250, NULL,
                                NULL, &stage) == 0);
    assert(stage == NIOS_TRANSACTION_COMPLETE);
    teardown(&usb);

    setup(&usb, &fns, &fake);
    nios_pkt_retune2_pack(request, BLADERF_MODULE_RX, NIOS_PKT_RETUNE2_NOW,
                          1, 0, 0x83, 0);
    nios_pkt_retune2_resp_pack(
        fake.scripted_responses[0], 0,
        NIOS_PKT_RETUNE2_RESP_FLAG_SUCCESS |
            NIOS_PKT_RETUNE2_RESP_FLAG_ASYNC_PENDING,
        NIOS_PKT_RETUNE2_FAILURE_NONE, 0, 0);
    fake.scripted_response_count = 1;
    assert(nios_usb_transaction(&usb, request, sizeof(request), 250, NULL,
                                NULL, &stage) == BLADERF_ERR_UNEXPECTED);
    assert(stage == NIOS_TRANSACTION_RESPONSE_MISMATCH);
    teardown(&usb);
}

static void test_scheduled_retune2_timeout_retains_and_drains_late_ack(void)
{
    struct bladerf_usb usb;
    struct usb_fns fns;
    struct fake_usb fake;
    enum nios_transaction_stage stage;
    uint8_t request[NIOS_PKT_LEN] = {0};

    setup(&usb, &fns, &fake);
    nios_pkt_8x32_pack(request,
                       NIOS_PKT_8x32_TARGET_COMMAND_SESSION_GENERATION,
                       false, 0, 0);
    nios_pkt_8x32_resp_pack(fake.scripted_responses[0],
                            NIOS_PKT_8x32_TARGET_COMMAND_SESSION_GENERATION,
                            false, 0, 0x01020304, true);
    fake.scripted_response_count = 1;
    assert(nios_usb_transaction(&usb, request, sizeof(request), 1000, NULL,
                                NULL, &stage) == 0);

    fake.fail_call = fake.calls + 2;
    nios_pkt_retune2_pack_token(request, BLADERF_MODULE_RX, 1000, 1, 0,
                                0x83, 0, 0x0022);
    assert(nios_usb_transaction(&usb, request, sizeof(request), 1000, NULL,
                                NULL, &stage) == BLADERF_ERR_TIMEOUT);
    assert(usb.peripheral->pending_retune2_response);
    assert(!usb.peripheral->desynchronized);

    /* Do not permit another state-changing tune while the original outcome
     * is ambiguous; read-only monitoring remains available. */
    nios_pkt_retune2_pack_token(request, BLADERF_MODULE_RX, 2000, 2, 0,
                                0x83, 0, 0x0023);
    assert(nios_usb_transaction(&usb, request, sizeof(request), 1000, NULL,
                                NULL, &stage) == BLADERF_ERR_WOULD_BLOCK);
    assert(fake.calls == 4);
    assert(usb.peripheral->pending_retune2_session_generation ==
           0x01020304);

    fake.fail_call = 0;
    nios_pkt_retune2_resp_pack(
        fake.scripted_responses[1], 0,
        NIOS_PKT_RETUNE2_RESP_FLAG_SUCCESS,
        NIOS_PKT_RETUNE2_FAILURE_NONE, 0x0022, 0x01020304);
    nios_pkt_8x32_resp_pack(
        fake.scripted_responses[2], NIOS_PKT_8x32_TARGET_RETUNE2_FAILURES,
        false, NIOS_PKT_8x32_RETUNE2_FAILURES_RX_ADDR, 0, true);
    fake.scripted_response_count = 3;
    nios_pkt_8x32_pack(request, NIOS_PKT_8x32_TARGET_RETUNE2_FAILURES,
                       false, NIOS_PKT_8x32_RETUNE2_FAILURES_RX_ADDR, 0);
    assert(nios_usb_transaction(&usb, request, sizeof(request), 1000, NULL,
                                NULL, &stage) == 0);
    assert(stage == NIOS_TRANSACTION_COMPLETE);
    assert(fake.scripted_response_index == 3);
    assert(!usb.peripheral->pending_retune2_response);
    assert(!usb.peripheral->desynchronized);
    teardown(&usb);
}

static void test_tokenless_retune2_timeout_recovers_after_session_reset(void)
{
    struct bladerf_usb usb;
    struct usb_fns fns;
    struct fake_usb fake;
    enum nios_transaction_stage stage;
    uint8_t request[NIOS_PKT_LEN] = {0};

    setup(&usb, &fns, &fake);
    nios_pkt_8x32_pack(request,
                       NIOS_PKT_8x32_TARGET_COMMAND_SESSION_GENERATION,
                       false, 0, 0);
    nios_pkt_8x32_resp_pack(fake.scripted_responses[0],
                            NIOS_PKT_8x32_TARGET_COMMAND_SESSION_GENERATION,
                            false, 0, 0x101, true);
    fake.scripted_response_count = 1;
    assert(nios_usb_transaction(&usb, request, sizeof(request), 1000, NULL,
                                NULL, &stage) == 0);

    fake.fail_call = fake.calls + 2;
    nios_pkt_retune2_pack_token(request, BLADERF_MODULE_RX,
                                NIOS_PKT_RETUNE2_NOW, 0, 1, 0x83, 0, 0x44);
    assert(nios_usb_transaction(&usb, request, sizeof(request), 1000, NULL,
                                NULL, &stage) == BLADERF_ERR_TIMEOUT);
    assert(usb.peripheral->pending_retune2_response);
    assert(!usb.peripheral->desynchronized);
    assert(usb.peripheral->pending_retune2_session_generation == 0x101);

    /* A session-generation read in the same session cannot prove that a
     * deferred tokenless operation's response has been retired. */
    fake.fail_call = 0;
    nios_pkt_8x32_resp_pack(fake.scripted_responses[1],
                            NIOS_PKT_8x32_TARGET_COMMAND_SESSION_GENERATION,
                            false, 0, 0x101, true);
    fake.scripted_response_count = 2;
    nios_pkt_8x32_pack(request,
                       NIOS_PKT_8x32_TARGET_COMMAND_SESSION_GENERATION,
                       false, 0, 0);
    assert(nios_usb_transaction(&usb, request, sizeof(request), 1000, NULL,
                                NULL, &stage) == 0);
    assert(usb.peripheral->pending_retune2_response);
    assert(!usb.peripheral->desynchronized);

    /* A confirmed new FPGA session has discarded every old-session reply. */
    nios_pkt_8x32_resp_pack(fake.scripted_responses[2],
                            NIOS_PKT_8x32_TARGET_COMMAND_SESSION_GENERATION,
                            false, 0, 0x102, true);
    fake.scripted_response_count = 3;
    assert(nios_usb_transaction(&usb, request, sizeof(request), 1000, NULL,
                                NULL, &stage) == 0);
    assert(!usb.peripheral->pending_retune2_response);
    assert(!usb.peripheral->desynchronized);
    assert(usb.peripheral->command_session_generation == 0x102);
    teardown(&usb);
}

static void test_tagged_write_partial_timeout_fails_closed(void)
{
    struct bladerf_usb usb;
    struct usb_fns fns;
    struct fake_usb fake;
    enum nios_transaction_stage stage;
    uint8_t buffer[NIOS_PKT_LEN] = {0};

    setup(&usb, &fns, &fake);
    fake.fail_call = 2;
    fake.timeout_transferred = 5;
    nios_pkt_16x64_pack_with_transaction_id(
        buffer, NIOS_PKT_16x64_TARGET_RFIC, true, 0x0104, 0x1234, 0x5a31);
    assert(nios_usb_transaction(&usb, buffer, sizeof(buffer), 250, NULL,
                                NULL, &stage) == BLADERF_ERR_TIMEOUT);
    assert(stage == NIOS_TRANSACTION_IN_FAILED);
    assert(!usb.peripheral->pending_rfic_write);
    assert(usb.peripheral->desynchronized);
    teardown(&usb);
}

static void test_tagged_write_unknown_timeout_length_fails_closed(void)
{
    struct bladerf_usb usb;
    struct usb_fns fns;
    struct fake_usb fake;
    enum nios_transaction_stage stage;
    uint8_t buffer[NIOS_PKT_LEN] = {0};

    setup(&usb, &fns, &fake);
    fake.fail_call = 2;
    fake.timeout_length_unknown = true;
    nios_pkt_16x64_pack_with_transaction_id(
        buffer, NIOS_PKT_16x64_TARGET_RFIC, true, 0x0104, 0x1234, 0x5a31);
    assert(nios_usb_transaction(&usb, buffer, sizeof(buffer), 250, NULL,
                                NULL, &stage) == BLADERF_ERR_UNEXPECTED);
    assert(stage == NIOS_TRANSACTION_IN_FAILED);
    assert(!usb.peripheral->pending_rfic_write);
    assert(usb.peripheral->desynchronized);
    teardown(&usb);
}

static void test_short_successful_in_reads_assemble_fixed_response(void)
{
    struct bladerf_usb usb;
    struct usb_fns fns;
    struct fake_usb fake;
    enum nios_transaction_stage stage;
    uint8_t buffer[NIOS_PKT_LEN] = {0};

    setup(&usb, &fns, &fake);
    fake.scripted_response_count = 1;
    fake.scripted_first_chunk_size = 6;
    nios_pkt_16x64_pack(buffer, NIOS_PKT_16x64_TARGET_RFIC, false, 0x42, 0);
    nios_pkt_16x64_resp_pack(fake.scripted_responses[0],
                             NIOS_PKT_16x64_TARGET_RFIC, false, 0x42,
                             0x12345678, true);
    assert(nios_usb_transaction(&usb, buffer, sizeof(buffer), 1000, NULL,
                                NULL, &stage) == 0);
    assert(stage == NIOS_TRANSACTION_COMPLETE);
    assert(fake.calls == 3);
    assert(fake.scripted_response_index == 1);
    teardown(&usb);
}

static void test_out_failure_does_not_read(void)
{
    struct bladerf_usb usb;
    struct usb_fns fns;
    struct fake_usb fake;
    enum nios_transaction_stage stage;
    uint8_t buffer[16] = { 0 };

    setup(&usb, &fns, &fake);
    fake.fail_call = 1;
    assert(nios_usb_transaction(&usb, buffer, sizeof(buffer), 250, NULL,
                                NULL, &stage) == BLADERF_ERR_TIMEOUT);
    assert(stage == NIOS_TRANSACTION_OUT_FAILED);
    assert(fake.calls == 1);
    assert(fake.endpoints[0] == PERIPHERAL_EP_OUT);
    assert(usb.peripheral->desynchronized);
    teardown(&usb);
}

static void test_matching_register_response_identity(void)
{
    struct bladerf_usb usb;
    struct usb_fns fns;
    struct fake_usb fake;
    enum nios_transaction_stage stage = NIOS_TRANSACTION_DESYNCHRONIZED;
    uint8_t buffer[NIOS_PKT_LEN] = {0};

    setup(&usb, &fns, &fake);
    fake.make_register_response = true;
    nios_pkt_16x64_pack(buffer, NIOS_PKT_16x64_TARGET_RFIC, false, 0x42,
                        0x12345678);
    assert(nios_usb_transaction(&usb, buffer, sizeof(buffer), 250,
                                NULL, NULL, &stage) == 0);
    assert(stage == NIOS_TRANSACTION_COMPLETE);
    assert(fake.calls == 2);
    assert(!usb.peripheral->desynchronized);
    teardown(&usb);
}

static void test_mismatched_register_response_fails_closed(void)
{
    struct bladerf_usb usb;
    struct usb_fns fns;
    struct fake_usb fake;
    enum nios_transaction_stage stage = NIOS_TRANSACTION_COMPLETE;
    uint8_t buffer[NIOS_PKT_LEN] = {0};

    setup(&usb, &fns, &fake);
    fake.make_register_response = true;
    fake.mutate_response_target = true;
    nios_pkt_16x64_pack(buffer, NIOS_PKT_16x64_TARGET_RFIC, false, 0x42,
                        0x12345678);
    assert(nios_usb_transaction(&usb, buffer, sizeof(buffer), 250,
                                NULL, NULL, &stage) == BLADERF_ERR_UNEXPECTED);
    assert(stage == NIOS_TRANSACTION_RESPONSE_MISMATCH);
    assert(fake.calls == 2);
    assert(usb.peripheral->desynchronized);

    /* Never let the late/misidentified response contaminate another request. */
    assert(nios_usb_transaction(&usb, buffer, sizeof(buffer), 250,
                                NULL, NULL, &stage) == BLADERF_ERR_UNEXPECTED);
    assert(stage == NIOS_TRANSACTION_DESYNCHRONIZED);
    assert(fake.calls == 2);
    teardown(&usb);
}

static void test_mismatched_16x64_write_payload_fails_closed(void)
{
    struct bladerf_usb usb;
    struct usb_fns fns;
    struct fake_usb fake;
    enum nios_transaction_stage stage = NIOS_TRANSACTION_COMPLETE;
    uint8_t buffer[NIOS_PKT_LEN] = {0};

    setup(&usb, &fns, &fake);
    fake.mutate_response_data = true;
    nios_pkt_16x64_pack(buffer, NIOS_PKT_16x64_TARGET_AD9361, true, 0x42,
                        UINT64_C(0x12345678));
    assert(nios_usb_transaction(&usb, buffer, sizeof(buffer), 250,
                                NULL, NULL, &stage) == BLADERF_ERR_UNEXPECTED);
    assert(stage == NIOS_TRANSACTION_RESPONSE_MISMATCH);
    assert(fake.calls == 2);
    assert(usb.peripheral->desynchronized);
    teardown(&usb);
}

static void test_tagged_rfic_write_response_echoes_transaction_id(void)
{
    struct bladerf_usb usb;
    struct usb_fns fns;
    struct fake_usb fake;
    enum nios_transaction_stage stage;
    uint8_t buffer[NIOS_PKT_LEN] = {0};

    setup(&usb, &fns, &fake);
    nios_pkt_16x64_pack_with_transaction_id(
        buffer, NIOS_PKT_16x64_TARGET_RFIC, true, 0x0104,
        UINT64_C(0x123456789abcdef0), 0x5a31);
    assert(nios_usb_transaction(&usb, buffer, sizeof(buffer), 250, NULL,
                                NULL, &stage) == 0);
    assert(stage == NIOS_TRANSACTION_COMPLETE);
    teardown(&usb);
}

static void test_tagged_rfic_write_rejects_wrong_transaction_id(void)
{
    struct bladerf_usb usb;
    struct usb_fns fns;
    struct fake_usb fake;
    enum nios_transaction_stage stage;
    uint8_t buffer[NIOS_PKT_LEN] = {0};

    setup(&usb, &fns, &fake);
    fake.mutate_response_reserved = true;
    fake.mutate_response_reserved_offset =
        NIOS_PKT_16x64_IDX_RFIC_TRANSACTION_ID;
    nios_pkt_16x64_pack_with_transaction_id(
        buffer, NIOS_PKT_16x64_TARGET_RFIC, true, 0x0104, 0x77, 0x5a31);
    assert(nios_usb_transaction(&usb, buffer, sizeof(buffer), 250, NULL,
                                NULL, &stage) == BLADERF_ERR_UNEXPECTED);
    assert(stage == NIOS_TRANSACTION_RESPONSE_MISMATCH);
    teardown(&usb);
}

static void test_tagged_rfic_timeout_drains_late_reply_before_status(void)
{
    struct bladerf_usb usb;
    struct bladerf_usb second_handle;
    struct usb_fns fns;
    struct usb_fns second_fns;
    struct fake_usb fake;
    struct fake_usb second_fake;
    enum nios_transaction_stage stage;
    uint8_t buffer[NIOS_PKT_LEN] = {0};

    setup(&usb, &fns, &fake);
    setup(&second_handle, &second_fns, &second_fake);
    /* Model two host handles attached to one serial-keyed endpoint state. */
    second_handle.peripheral = usb.peripheral;
    fake.fail_call = 2;
    nios_pkt_16x64_pack_with_transaction_id(
        buffer, NIOS_PKT_16x64_TARGET_RFIC, true, 0x0104,
        UINT64_C(0x123456789abcdef0), 0x5a31);
    assert(nios_usb_transaction(&usb, buffer, sizeof(buffer), 1000, NULL,
                                NULL, &stage) == BLADERF_ERR_TIMEOUT);
    assert(stage == NIOS_TRANSACTION_IN_FAILED);
    assert(usb.peripheral->pending_rfic_write);
    assert(!usb.peripheral->desynchronized);

    memset(buffer, 0, sizeof(buffer));
    assert(nios_usb_transaction(&second_handle, buffer, sizeof(buffer), 1000, NULL,
                                NULL, &stage) == BLADERF_ERR_WOULD_BLOCK);
    assert(stage == NIOS_TRANSACTION_PENDING_RESPONSE);
    assert(fake.calls == 2);
    assert(second_fake.calls == 0);

    fake.fail_call = 0;
    nios_pkt_16x64_resp_pack_with_transaction_id(
        fake.scripted_responses[0], NIOS_PKT_16x64_TARGET_RFIC, true,
        0x0104, UINT64_C(0x123456789abcdef0), true, 0x5a31);
    nios_pkt_16x64_resp_pack(
        fake.scripted_responses[1], NIOS_PKT_16x64_TARGET_RFIC, false,
        0x0f0c, UINT64_C(0x0000000100015a31), true);
    fake.scripted_response_count = 2;
    nios_pkt_16x64_pack(buffer, NIOS_PKT_16x64_TARGET_RFIC, false, 0x0f0c,
                        0);

    assert(nios_usb_transaction(&usb, buffer, sizeof(buffer), 1000, NULL,
                                NULL, &stage) == 0);
    assert(stage == NIOS_TRANSACTION_COMPLETE);
    assert(fake.scripted_response_index == 2);
    assert(!usb.peripheral->pending_rfic_write);
    assert(!usb.peripheral->desynchronized);
    second_handle.peripheral = &second_handle.peripheral_local;
    teardown(&second_handle);
    teardown(&usb);
}

static void test_tagged_rfic_response_lost_across_session_reset(void)
{
    struct bladerf_usb usb;
    struct usb_fns fns;
    struct fake_usb fake;
    enum nios_transaction_stage stage;
    uint8_t request[NIOS_PKT_LEN] = {0};

    setup(&usb, &fns, &fake);
    nios_pkt_16x64_pack_with_transaction_id(
        request, NIOS_PKT_16x64_TARGET_RFIC, true, 0x0104,
        UINT64_C(0x123456789abcdef0), 0x6b42);
    fake.fail_call = 2;
    fake.timeout_transferred = 0;
    assert(nios_usb_transaction(&usb, request, sizeof(request), 1000, NULL,
                                NULL, &stage) == BLADERF_ERR_TIMEOUT);
    assert(usb.peripheral->pending_rfic_write);
    assert(!usb.peripheral->desynchronized);

    /* Model FX3 SYS_RST: the old UART response is flushed, but the retained
     * NIOS terminal result remains queryable. The host must reconcile by
     * status and must not replay the possibly completed RFIC mutation. */
    fake.fail_call = 0;
    fake.scripted_response_count = 1;
    nios_pkt_16x64_resp_pack(
        fake.scripted_responses[0], NIOS_PKT_16x64_TARGET_RFIC, false,
        0x0f0c, UINT64_C(0x0000000100016b42), true);
    nios_pkt_16x64_pack(request, NIOS_PKT_16x64_TARGET_RFIC, false,
                        0x0f0c, 0);
    assert(nios_usb_transaction(&usb, request, sizeof(request), 1000, NULL,
                                NULL, &stage) == 0);
    assert(stage == NIOS_TRANSACTION_COMPLETE);
    assert(fake.calls == 4);
    assert(fake.scripted_response_index == 1);
    assert(fake.requests[2][NIOS_PKT_16x64_IDX_TARGET_ID] ==
           NIOS_PKT_16x64_TARGET_RFIC);
    assert((fake.requests[2][NIOS_PKT_16x64_IDX_FLAGS] &
            NIOS_PKT_16x64_FLAG_WRITE) == 0);
    assert(fake.requests[2][NIOS_PKT_16x64_IDX_ADDR] == 0x0c &&
           fake.requests[2][NIOS_PKT_16x64_IDX_ADDR + 1] == 0x0f);
    assert(!usb.peripheral->pending_rfic_write);
    assert(!usb.peripheral->desynchronized);
    teardown(&usb);
}

static void test_tagged_rfic_retry_drains_duplicate_ack_before_status(void)
{
    struct bladerf_usb usb;
    struct usb_fns fns;
    struct fake_usb fake;
    enum nios_transaction_stage stage;
    uint8_t buffer[NIOS_PKT_LEN] = {0};

    setup(&usb, &fns, &fake);
    nios_pkt_16x64_pack_with_transaction_id(
        buffer, NIOS_PKT_16x64_TARGET_RFIC, true, 0x0104, 0x12345678,
        0x5a31);
    fake.fail_call = 2;
    assert(nios_usb_transaction(&usb, buffer, sizeof(buffer), 1000, NULL,
                                NULL, &stage) == BLADERF_ERR_TIMEOUT);
    assert(usb.peripheral->pending_rfic_write);

    /* The original ACK may still be active when NIOS accepts the exact
     * retry. NIOS emits a second, identical ACK for that idempotent retry. */
    fake.fail_call = 0;
    nios_pkt_16x64_resp_pack_with_transaction_id(
        fake.scripted_responses[0], NIOS_PKT_16x64_TARGET_RFIC, true,
        0x0104, 0x12345678, true, 0x5a31);
    nios_pkt_16x64_resp_pack_with_transaction_id(
        fake.scripted_responses[1], NIOS_PKT_16x64_TARGET_RFIC, true,
        0x0104, 0x12345678, true, 0x5a31);
    nios_pkt_16x64_resp_pack(
        fake.scripted_responses[2], NIOS_PKT_16x64_TARGET_RFIC, false,
        0x0f0c, UINT64_C(0x0000000100015a31), true);
    fake.scripted_response_count = 3;

    nios_pkt_16x64_pack_with_transaction_id(
        buffer, NIOS_PKT_16x64_TARGET_RFIC, true, 0x0104, 0x12345678,
        0x5a31);
    assert(nios_usb_transaction(&usb, buffer, sizeof(buffer), 1000, NULL,
                                NULL, &stage) == 0);
    assert(stage == NIOS_TRANSACTION_COMPLETE);
    assert(fake.scripted_response_index == 1);
    assert(usb.peripheral->pending_rfic_write);
    assert(!usb.peripheral->desynchronized);

    /* The following read drains the second ACK first, then accepts the
     * ordered status reply and clears the ambiguous pending identity. */
    nios_pkt_16x64_pack(buffer, NIOS_PKT_16x64_TARGET_RFIC, false, 0x0f0c,
                        0);
    assert(nios_usb_transaction(&usb, buffer, sizeof(buffer), 1000, NULL,
                                NULL, &stage) == 0);
    assert(stage == NIOS_TRANSACTION_COMPLETE);
    assert(fake.scripted_response_index == 3);
    assert(!usb.peripheral->pending_rfic_write);
    assert(!usb.peripheral->desynchronized);
    teardown(&usb);
}

static void test_tagged_rfic_retry_without_duplicate_clears_on_status(void)
{
    struct bladerf_usb usb;
    struct usb_fns fns;
    struct fake_usb fake;
    enum nios_transaction_stage stage;
    uint8_t buffer[NIOS_PKT_LEN] = {0};

    setup(&usb, &fns, &fake);
    nios_pkt_16x64_pack_with_transaction_id(
        buffer, NIOS_PKT_16x64_TARGET_RFIC, true, 0x0104, 0x12345678,
        0x5a31);
    fake.fail_call = 2;
    assert(nios_usb_transaction(&usb, buffer, sizeof(buffer), 1000, NULL,
                                NULL, &stage) == BLADERF_ERR_TIMEOUT);
    assert(usb.peripheral->pending_rfic_write);

    fake.fail_call = 0;
    nios_pkt_16x64_resp_pack_with_transaction_id(
        fake.scripted_responses[0], NIOS_PKT_16x64_TARGET_RFIC, true,
        0x0104, 0x12345678, true, 0x5a31);
    nios_pkt_16x64_resp_pack(
        fake.scripted_responses[1], NIOS_PKT_16x64_TARGET_RFIC, false,
        0x0f0c, UINT64_C(0x0000000100015a31), true);
    fake.scripted_response_count = 2;

    nios_pkt_16x64_pack_with_transaction_id(
        buffer, NIOS_PKT_16x64_TARGET_RFIC, true, 0x0104, 0x12345678,
        0x5a31);
    assert(nios_usb_transaction(&usb, buffer, sizeof(buffer), 1000, NULL,
                                NULL, &stage) == 0);
    assert(usb.peripheral->pending_rfic_write);

    /* If the original ACK was actually lost, the retry's only ACK is
     * ambiguous too. The ordered status reply still resolves the pending
     * identity without requiring a nonexistent second ACK. */
    nios_pkt_16x64_pack(buffer, NIOS_PKT_16x64_TARGET_RFIC, false, 0x0f0c,
                        0);
    assert(nios_usb_transaction(&usb, buffer, sizeof(buffer), 1000, NULL,
                                NULL, &stage) == 0);
    assert(fake.scripted_response_index == 2);
    assert(!usb.peripheral->pending_rfic_write);
    assert(!usb.peripheral->desynchronized);
    teardown(&usb);
}

static void test_status_timeout_with_pending_write_retains_both_responses(void)
{
    struct bladerf_usb usb;
    struct usb_fns fns;
    struct fake_usb fake;
    enum nios_transaction_stage stage;
    uint8_t buffer[NIOS_PKT_LEN] = {0};

    setup(&usb, &fns, &fake);
    fake.fail_call = 2;
    nios_pkt_16x64_pack_with_transaction_id(
        buffer, NIOS_PKT_16x64_TARGET_RFIC, true, 0x0104, 0x1234, 0x0012);
    assert(nios_usb_transaction(&usb, buffer, sizeof(buffer), 1000, NULL,
                                NULL, &stage) == BLADERF_ERR_TIMEOUT);
    assert(usb.peripheral->pending_rfic_write);

    nios_pkt_16x64_pack(buffer, NIOS_PKT_16x64_TARGET_RFIC, false, 0x0f0c,
                        0);
    fake.fail_call = 4;
    assert(nios_usb_transaction(&usb, buffer, sizeof(buffer), 1000, NULL,
                                NULL, &stage) == BLADERF_ERR_TIMEOUT);
    assert(stage == NIOS_TRANSACTION_IN_FAILED);
    assert(usb.peripheral->pending_rfic_write);
    assert(usb.peripheral->pending_read_response);
    assert(!usb.peripheral->desynchronized);
    teardown(&usb);
}


static void test_nonzero_16x64_reserved_response_fails_closed(void)
{
    struct bladerf_usb usb;
    struct usb_fns fns;
    struct fake_usb fake;
    enum nios_transaction_stage stage = NIOS_TRANSACTION_COMPLETE;
    uint8_t buffer[NIOS_PKT_LEN] = {0};

    setup(&usb, &fns, &fake);
    fake.mutate_response_reserved = true;
    fake.mutate_response_reserved_offset = NIOS_PKT_16x64_IDX_RESV1;
    nios_pkt_16x64_pack(buffer, NIOS_PKT_16x64_TARGET_AD9361, false, 0x42, 0);
    assert(nios_usb_transaction(&usb, buffer, sizeof(buffer), 250,
                                NULL, NULL, &stage) == BLADERF_ERR_UNEXPECTED);
    assert(stage == NIOS_TRANSACTION_RESPONSE_MISMATCH);
    assert(usb.peripheral->desynchronized);
    teardown(&usb);
}


static void test_8x8_reserved_tail_fails_closed(void)
{
    struct bladerf_usb usb;
    struct usb_fns fns;
    struct fake_usb fake;
    enum nios_transaction_stage stage = NIOS_TRANSACTION_COMPLETE;
    uint8_t buffer[NIOS_PKT_LEN] = {0};

    setup(&usb, &fns, &fake);
    fake.mutate_response_reserved = true;
    fake.mutate_response_reserved_offset = NIOS_PKT_8x8_IDX_RESV2;
    nios_pkt_8x8_pack(buffer, NIOS_PKT_8x8_TARGET_VCTCXO_TAMER,
                       false, 0, 0);
    assert(nios_usb_transaction(&usb, buffer, sizeof(buffer), 250,
                                NULL, NULL, &stage) == BLADERF_ERR_UNEXPECTED);
    assert(stage == NIOS_TRANSACTION_RESPONSE_MISMATCH);
    assert(usb.peripheral->desynchronized);
    teardown(&usb);
}

static void test_16x64_batch_result_is_not_compared_as_payload(void)
{
    struct bladerf_usb usb;
    struct usb_fns fns;
    struct fake_usb fake;
    enum nios_transaction_stage stage = NIOS_TRANSACTION_DESYNCHRONIZED;
    uint8_t buffer[NIOS_PKT_LEN] = {0};

    setup(&usb, &fns, &fake);
    fake.make_register_response = true;
    nios_pkt_16x64_pack(buffer, NIOS_PKT_16x64_TARGET_AD9361_WRITE_BATCH,
                        true, 2, UINT64_C(0xaabbccdd));
    assert(nios_usb_transaction(&usb, buffer, sizeof(buffer), 250,
                                NULL, NULL, &stage) == 0);
    assert(stage == NIOS_TRANSACTION_COMPLETE);
    assert(!usb.peripheral->desynchronized);
    teardown(&usb);
}


static void test_retune_reserved_flags_fail_closed(void)
{
    struct bladerf_usb usb;
    struct usb_fns fns;
    struct fake_usb fake;
    enum nios_transaction_stage stage = NIOS_TRANSACTION_COMPLETE;
    uint8_t buffer[NIOS_PKT_LEN] = {0};

    setup(&usb, &fns, &fake);
    fake.mutate_response_flags = true;
    fake.mutate_response_flags_offset = NIOS_PKT_RETUNERESP_IDX_FLAGS;
    nios_pkt_retune_pack(buffer, BLADERF_MODULE_RX, 0, 0, 0, 0, 0, false,
                         0, false);
    assert(nios_usb_transaction(&usb, buffer, sizeof(buffer), 250,
                                NULL, NULL, &stage) == BLADERF_ERR_UNEXPECTED);
    assert(stage == NIOS_TRANSACTION_RESPONSE_MISMATCH);
    assert(usb.peripheral->desynchronized);
    teardown(&usb);
}

static void test_retune2_reserved_tail_fails_closed(void)
{
    struct bladerf_usb usb;
    struct usb_fns fns;
    struct fake_usb fake;
    enum nios_transaction_stage stage = NIOS_TRANSACTION_COMPLETE;
    uint8_t buffer[NIOS_PKT_LEN] = {0};

    setup(&usb, &fns, &fake);
    fake.mutate_response_reserved = true;
    fake.mutate_response_reserved_offset = NIOS_PKT_RETUNE2_RESP_IDX_RESV;
    nios_pkt_retune2_pack(buffer, BLADERF_MODULE_RX, 0, 0, 0, 0, 0);
    assert(nios_usb_transaction(&usb, buffer, sizeof(buffer), 250,
                                NULL, NULL, &stage) == BLADERF_ERR_UNEXPECTED);
    assert(stage == NIOS_TRANSACTION_RESPONSE_MISMATCH);
    assert(usb.peripheral->desynchronized);
    teardown(&usb);
}

static void test_retune_timestamp_valid_flag_matches_request(void)
{
    struct bladerf_usb usb;
    struct usb_fns fns;
    struct fake_usb fake;
    enum nios_transaction_stage stage = NIOS_TRANSACTION_COMPLETE;
    uint8_t buffer[NIOS_PKT_LEN] = {0};

    setup(&usb, &fns, &fake);
    fake.make_semantic_retune_response = true;
    nios_pkt_retune_pack(buffer, BLADERF_MODULE_RX, 123, 0, 0, 0, 0, false,
                         0, false);
    assert(nios_usb_transaction(&usb, buffer, sizeof(buffer), 250,
                                NULL, NULL, &stage) == BLADERF_ERR_UNEXPECTED);
    assert(stage == NIOS_TRANSACTION_RESPONSE_MISMATCH);
    teardown(&usb);
}

static void test_retune2_timestamp_valid_flag_matches_request(void)
{
    struct bladerf_usb usb;
    struct usb_fns fns;
    struct fake_usb fake;
    enum nios_transaction_stage stage = NIOS_TRANSACTION_COMPLETE;
    uint8_t buffer[NIOS_PKT_LEN] = {0};

    setup(&usb, &fns, &fake);
    fake.make_semantic_retune2_response = true;
    nios_pkt_retune2_pack(buffer, BLADERF_MODULE_RX, 123, 0, 0, 0, 0);
    assert(nios_usb_transaction(&usb, buffer, sizeof(buffer), 250,
                                NULL, NULL, &stage) == BLADERF_ERR_UNEXPECTED);
    assert(stage == NIOS_TRANSACTION_RESPONSE_MISMATCH);
    teardown(&usb);
}

static void test_retune_now_timestamp_valid_responses_are_accepted(void)
{
    struct bladerf_usb usb;
    struct usb_fns fns;
    struct fake_usb fake;
    enum nios_transaction_stage stage = NIOS_TRANSACTION_DESYNCHRONIZED;
    uint8_t buffer[NIOS_PKT_LEN] = {0};

    setup(&usb, &fns, &fake);
    fake.make_semantic_retune_response = true;
    nios_pkt_retune_pack(buffer, BLADERF_MODULE_RX, NIOS_PKT_RETUNE_NOW,
                         0, 0, 0, 0, false, 0, false);
    assert(nios_usb_transaction(&usb, buffer, sizeof(buffer), 250,
                                NULL, NULL, &stage) == 0);
    assert(stage == NIOS_TRANSACTION_COMPLETE);
    teardown(&usb);

    setup(&usb, &fns, &fake);
    fake.make_semantic_retune2_response = true;
    nios_pkt_retune2_pack(buffer, BLADERF_MODULE_RX, NIOS_PKT_RETUNE2_NOW,
                          0, 0, 0, 0);
    assert(nios_usb_transaction(&usb, buffer, sizeof(buffer), 250,
                                NULL, NULL, &stage) == 0);
    assert(stage == NIOS_TRANSACTION_COMPLETE);
    teardown(&usb);
}

static void test_legacy_config_normalized_address_is_validated(void)
{
    struct bladerf_usb usb;
    struct usb_fns fns;
    struct fake_usb fake;
    enum nios_transaction_stage stage = NIOS_TRANSACTION_DESYNCHRONIZED;
    uint8_t buffer[NIOS_PKT_LEN] = {0};

    setup(&usb, &fns, &fake);
    fake.make_legacy_config_response = true;
    buffer[0] = NIOS_PKT_LEGACY_MAGIC;
    buffer[1] = NIOS_PKT_LEGACY_MODE_DIR_READ |
                NIOS_PKT_LEGACY_DEV_CONFIG | 1;
    buffer[2] = NIOS_PKT_LEGACY_PIO_ADDR_IQ_RX_GAIN;
    assert(nios_usb_transaction(&usb, buffer, sizeof(buffer), 250,
                                NULL, NULL, &stage) == 0);
    assert(stage == NIOS_TRANSACTION_COMPLETE);
    assert(!usb.peripheral->desynchronized);
    teardown(&usb);
}

static void test_mismatched_legacy_config_response_fails_closed(void)
{
    struct bladerf_usb usb;
    struct usb_fns fns;
    struct fake_usb fake;
    enum nios_transaction_stage stage = NIOS_TRANSACTION_COMPLETE;
    uint8_t buffer[NIOS_PKT_LEN] = {0};

    setup(&usb, &fns, &fake);
    fake.make_legacy_config_response = true;
    fake.mutate_legacy_config_response_addr = true;
    buffer[0] = NIOS_PKT_LEGACY_MAGIC;
    buffer[1] = NIOS_PKT_LEGACY_MODE_DIR_READ |
                NIOS_PKT_LEGACY_DEV_CONFIG | 1;
    buffer[2] = NIOS_PKT_LEGACY_PIO_ADDR_IQ_RX_GAIN;
    assert(nios_usb_transaction(&usb, buffer, sizeof(buffer), 250,
                                NULL, NULL, &stage) == BLADERF_ERR_UNEXPECTED);
    assert(stage == NIOS_TRANSACTION_RESPONSE_MISMATCH);
    assert(usb.peripheral->desynchronized);
    teardown(&usb);
}

static void test_legacy_error_marker_remains_a_valid_response(void)
{
    struct bladerf_usb usb;
    struct usb_fns fns;
    struct fake_usb fake;
    enum nios_transaction_stage stage = NIOS_TRANSACTION_DESYNCHRONIZED;
    uint8_t buffer[NIOS_PKT_LEN] = {0};

    setup(&usb, &fns, &fake);
    fake.make_legacy_error_response = true;
    buffer[0] = NIOS_PKT_LEGACY_MAGIC;
    buffer[1] = NIOS_PKT_LEGACY_MODE_DIR_READ |
                NIOS_PKT_LEGACY_DEV_CONFIG | 1;
    buffer[2] = NIOS_PKT_LEGACY_PIO_ADDR_IQ_RX_GAIN;
    assert(nios_usb_transaction(&usb, buffer, sizeof(buffer), 250,
                                NULL, NULL, &stage) == 0);
    assert(stage == NIOS_TRANSACTION_COMPLETE);
    assert(!usb.peripheral->desynchronized);
    teardown(&usb);
}

static void test_nios_poll_deadline_fails_closed_on_clock_error(void)
{
    uint64_t deadline = UINT64_C(0xfeedface);
    bool expired = true;

    assert(!nios_deadline_start(-1, 100, 1000, &deadline));
    assert(deadline == UINT64_C(0xfeedface));
    assert(nios_deadline_start(0, 100, 1000, &deadline));
    assert(deadline == 1100);
    assert(nios_deadline_check(0, 1099, deadline, &expired));
    assert(!expired);
    assert(nios_deadline_check(0, 1100, deadline, &expired));
    assert(expired);
    assert(!nios_deadline_check(-1, 1101, deadline, &expired));
    assert(!nios_deadline_start(0, UINT64_MAX - 5, 10, &deadline));
}

int main(void)
{
    test_retune2_new_failure_stages_round_trip();
    test_retune2_response_echoes_request_token();
    test_tokened_now_async_acceptance_response();
    test_scheduled_retune2_timeout_retains_and_drains_late_ack();
    test_tokenless_retune2_timeout_recovers_after_session_reset();
    test_success();
    test_in_timeout_fails_closed();
    test_zero_byte_read_timeout_recovers_through_ordered_barrier();
    test_zero_byte_read_timeout_recovers_when_original_was_lost();
    test_ordered_barrier_timeout_remains_fail_closed();
    test_read_barrier_drains_pending_rfic_duplicate_acks();
    test_tagged_write_full_ack_with_timeout_status_is_consumed();
    test_tagged_rfic_busy_response_is_a_valid_terminal_reply();
    test_retune_busy_responses_are_terminal_and_correlated();
    test_tagged_write_partial_timeout_fails_closed();
    test_tagged_write_unknown_timeout_length_fails_closed();
    test_short_successful_in_reads_assemble_fixed_response();
    test_out_failure_does_not_read();
    test_matching_register_response_identity();
    test_mismatched_register_response_fails_closed();
    test_mismatched_16x64_write_payload_fails_closed();
    test_tagged_rfic_write_response_echoes_transaction_id();
    test_tagged_rfic_write_rejects_wrong_transaction_id();
    test_tagged_rfic_timeout_drains_late_reply_before_status();
    test_tagged_rfic_response_lost_across_session_reset();
    test_tagged_rfic_retry_drains_duplicate_ack_before_status();
    test_tagged_rfic_retry_without_duplicate_clears_on_status();
    test_status_timeout_with_pending_write_retains_both_responses();
    test_nonzero_16x64_reserved_response_fails_closed();
    test_8x8_reserved_tail_fails_closed();
    test_16x64_batch_result_is_not_compared_as_payload();
    test_retune_reserved_flags_fail_closed();
    test_retune2_reserved_tail_fails_closed();
    test_retune_timestamp_valid_flag_matches_request();
    test_retune2_timestamp_valid_flag_matches_request();
    test_retune_now_timestamp_valid_responses_are_accepted();
    test_legacy_config_normalized_address_is_validated();
    test_mismatched_legacy_config_response_fails_closed();
    test_legacy_error_marker_remains_a_valid_response();
    test_nios_poll_deadline_fails_closed_on_clock_error();
    puts("NIOS USB transaction fail-closed tests: PASS");
    return 0;
}
