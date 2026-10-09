#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "backend/usb/nios_transaction.h"
#include "libbladeRF.h"

struct fake_usb {
    int calls;
    int fail_call;
    uint8_t endpoints[4];
    uint32_t timeouts[4];
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

int main(void)
{
    test_success();
    test_in_timeout_fails_closed();
    test_out_failure_does_not_read();
    puts("NIOS USB transaction fail-closed tests: PASS");
    return 0;
}
