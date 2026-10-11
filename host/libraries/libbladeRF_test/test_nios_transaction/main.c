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
    uint8_t endpoints[4];
    uint32_t timeouts[4];
    bool make_register_response;
    bool mutate_response_target;
    bool mutate_response_data;
    bool mutate_response_reserved;
    uint8_t mutate_response_reserved_offset;
    bool mutate_response_flags;
    uint8_t mutate_response_flags_offset;
};

static int fake_bulk_transfer(void *driver, uint8_t endpoint, void *buffer,
                              uint32_t length, uint32_t timeout_ms)
{
    struct fake_usb *fake = driver;
    int call = fake->calls++;
    (void)buffer;
    (void)length;

    assert(call < 4);
    fake->endpoints[call] = endpoint;
    fake->timeouts[call] = timeout_ms;
    if (endpoint == PERIPHERAL_EP_IN && fake->make_register_response) {
        uint8_t *response = buffer;
        response[NIOS_PKT_16x64_IDX_FLAGS] |=
            NIOS_PKT_16x64_FLAG_SUCCESS;
        /* Reads return data different from the request. Identity validation
         * must compare only the transaction key, not response payload. */
        response[NIOS_PKT_16x64_IDX_DATA] = 0x5a;
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
    fns->bulk_transfer = fake_bulk_transfer;
    usb->fn = fns;
    usb->driver = fake;
    assert(MUTEX_INIT(&usb->peripheral_lock) == 0);
}

static void teardown(struct bladerf_usb *usb)
{
    MUTEX_DESTROY(&usb->peripheral_lock);
}

static void test_success(void)
{
    struct bladerf_usb usb;
    struct usb_fns fns;
    struct fake_usb fake;
    enum nios_transaction_stage stage = NIOS_TRANSACTION_DESYNCHRONIZED;
    uint8_t buffer[16] = { 0 };

    _Static_assert(NIOS_RFIC_RESPONSE_TIMEOUT_MS > 55000u,
                   "RFIC response budget must exceed the preserved 55 s block");

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
    assert(!usb.peripheral_desynchronized);
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
    assert(usb.peripheral_desynchronized);

    assert(nios_usb_transaction(&usb, buffer, sizeof(buffer),
                                NIOS_RFIC_RESPONSE_TIMEOUT_MS, NULL, NULL,
                                &stage) == BLADERF_ERR_UNEXPECTED);
    assert(stage == NIOS_TRANSACTION_DESYNCHRONIZED);
    assert(fake.calls == 2);
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
    assert(usb.peripheral_desynchronized);
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
    assert(!usb.peripheral_desynchronized);
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
    assert(usb.peripheral_desynchronized);

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
    assert(usb.peripheral_desynchronized);
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
    assert(usb.peripheral_desynchronized);
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
    assert(usb.peripheral_desynchronized);
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
    assert(!usb.peripheral_desynchronized);
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
    assert(usb.peripheral_desynchronized);
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
    assert(usb.peripheral_desynchronized);
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
    test_success();
    test_in_timeout_fails_closed();
    test_out_failure_does_not_read();
    test_matching_register_response_identity();
    test_mismatched_register_response_fails_closed();
    test_mismatched_16x64_write_payload_fails_closed();
    test_nonzero_16x64_reserved_response_fails_closed();
    test_8x8_reserved_tail_fails_closed();
    test_16x64_batch_result_is_not_compared_as_payload();
    test_retune_reserved_flags_fail_closed();
    test_retune2_reserved_tail_fails_closed();
    test_nios_poll_deadline_fails_closed_on_clock_error();
    puts("NIOS USB transaction fail-closed tests: PASS");
    return 0;
}
