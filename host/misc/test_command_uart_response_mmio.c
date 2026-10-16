#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "command_uart_response_mmio.h"
#include "command_uart_response_publish.h"

#define MAX_TRACE 8

struct mmio_fake {
    uint32_t session_generation;
    uint32_t status;
    uint32_t read_offsets[MAX_TRACE];
    size_t read_count;
    uint32_t write_offsets[MAX_TRACE];
    uint32_t write_values[MAX_TRACE];
    size_t write_count;
};

static uint32_t fake_read32(void *context, uint32_t offset)
{
    struct mmio_fake *fake = context;

    if (fake->read_count >= MAX_TRACE) {
        return UINT32_MAX;
    }
    fake->read_offsets[fake->read_count++] = offset;
    if (offset == COMMAND_UART_RESPONSE_SESSION_OFFSET) {
        return fake->session_generation;
    }
    if (offset == COMMAND_UART_RESPONSE_STATUS_OFFSET) {
        return fake->status;
    }
    return UINT32_MAX;
}

static void fake_write32(void *context, uint32_t offset, uint32_t value)
{
    struct mmio_fake *fake = context;

    if (fake->write_count >= MAX_TRACE) {
        return;
    }
    fake->write_offsets[fake->write_count] = offset;
    fake->write_values[fake->write_count] = value;
    ++fake->write_count;
    if (offset == COMMAND_UART_RESPONSE_SESSION_OFFSET) {
        fake->session_generation = value;
    }
}

static bool no_writes(const struct mmio_fake *fake)
{
    return fake->write_count == 0;
}

static bool test_stale_session(void)
{
    struct mmio_fake fake = { .session_generation = 8 };
    const struct command_uart_response_mmio mmio = {
        fake_read32, fake_write32, &fake
    };
    uint8_t response[16] = { 0 };

    return command_uart_response_mmio_try_publish(&mmio, response, 7) ==
               COMMAND_UART_TRY_PUBLISH_STALE &&
           fake.read_count == 1 &&
           fake.read_offsets[0] == COMMAND_UART_RESPONSE_SESSION_OFFSET &&
           no_writes(&fake);
}

static bool test_slot_rejection(uint32_t status,
                                enum command_uart_try_publish_result expected)
{
    struct mmio_fake fake = { .session_generation = 9, .status = status };
    const struct command_uart_response_mmio mmio = {
        fake_read32, fake_write32, &fake
    };
    uint8_t response[16] = { 0 };

    return command_uart_response_mmio_try_publish(&mmio, response, 9) ==
               expected &&
           fake.read_count == 2 &&
           fake.read_offsets[0] == COMMAND_UART_RESPONSE_SESSION_OFFSET &&
           fake.read_offsets[1] == COMMAND_UART_RESPONSE_STATUS_OFFSET &&
           no_writes(&fake);
}

static bool test_frame_commit_and_retry(void)
{
    static const uint8_t response[16] = {
        0x45, 0x02, 0x81, 0x7f, 0x10, 0x20, 0x30, 0x40,
        0x00, 0x01, 0xfe, 0xff, 0xaa, 0xbb, 0xcc, 0xdd
    };
    static const uint32_t expected_offsets[5] = { 24, 0, 4, 8, 12 };
    static const uint32_t expected_values[5] = {
        0x12345678, 0x7f810245, 0x40302010,
        0xfffe0100, 0xddccbbaa
    };
    struct mmio_fake fake = {
        .session_generation = 0x12345678,
        .status = COMMAND_UART_STATUS_RESPONSE_PENDING
    };
    const struct command_uart_response_mmio mmio = {
        fake_read32, fake_write32, &fake
    };
    size_t i;

    if (command_uart_response_mmio_try_publish(
            &mmio, response, fake.session_generation) !=
            COMMAND_UART_TRY_PUBLISH_BACKPRESSURE ||
        !no_writes(&fake)) {
        return false;
    }

    fake.status = 0;
    fake.read_count = 0;
    if (command_uart_response_mmio_try_publish(
            &mmio, response, fake.session_generation) !=
            COMMAND_UART_TRY_PUBLISH_OK ||
        fake.write_count != 5) {
        return false;
    }

    for (i = 0; i < 5; ++i) {
        if (fake.write_offsets[i] != expected_offsets[i] ||
            fake.write_values[i] != expected_values[i]) {
            return false;
        }
    }
    return true;
}

struct publish_fake {
    uint32_t session_reads[2];
    size_t session_read_count;
    enum command_uart_try_publish_result write_result;
    size_t write_count;
    size_t disable_count;
    size_t enable_count;
};

static uint32_t publish_fake_read_session(void *context)
{
    struct publish_fake *fake = context;
    size_t index = fake->session_read_count++;

    if (index >= 2) {
        index = 1;
    }
    return fake->session_reads[index];
}

static enum command_uart_try_publish_result publish_fake_write(
    void *context, uint8_t *response, uint32_t session_generation)
{
    struct publish_fake *fake = context;

    (void)response;
    (void)session_generation;
    ++fake->write_count;
    return fake->write_result;
}

static void publish_fake_set_isr(void *context, bool enabled)
{
    struct publish_fake *fake = context;

    if (enabled) {
        ++fake->enable_count;
    } else {
        ++fake->disable_count;
    }
}

static bool test_publish_fault_and_session_recovery(void)
{
    uint8_t response[16] = { 0 };
    struct command_uart_fault_state fault = { 0 };
    struct publish_fake fake = {
        .session_reads = { 20, 20 },
        .write_result = COMMAND_UART_TRY_PUBLISH_BACKPRESSURE,
    };
    const struct command_uart_response_publish_ops ops = {
        publish_fake_read_session,
        publish_fake_write,
        publish_fake_set_isr,
        &fake,
    };

    if (command_uart_response_publish_try(&ops, &fault, response, 19) !=
            COMMAND_UART_TRY_PUBLISH_STALE ||
        fake.write_count != 0 || fault.latched || fake.disable_count != 0) {
        return false;
    }

    fake.session_read_count = 0;
    if (command_uart_response_publish_try(&ops, &fault, response, 20) !=
            COMMAND_UART_TRY_PUBLISH_BACKPRESSURE ||
        fake.write_count != 1 || fault.latched || fake.disable_count != 0) {
        return false;
    }

    fake.session_read_count = 0;
    fake.write_result = COMMAND_UART_TRY_PUBLISH_ERROR;
    if (command_uart_response_publish_try(&ops, &fault, response, 20) !=
            COMMAND_UART_TRY_PUBLISH_ERROR ||
        !fault.latched || fault.session_generation != 20 ||
        fake.disable_count != 1 || fake.enable_count != 0) {
        return false;
    }

    if (command_uart_response_publish_recover(&ops, &fault, 20) ||
        !fault.latched || fake.enable_count != 0 ||
        !command_uart_response_publish_recover(&ops, &fault, 21) ||
        fault.latched || fake.enable_count != 1) {
        return false;
    }

    fake.session_reads[0] = 30;
    fake.session_reads[1] = 31;
    fake.session_read_count = 0;
    fake.write_result = COMMAND_UART_TRY_PUBLISH_ERROR;
    if (command_uart_response_publish_try(&ops, &fault, response, 30) !=
            COMMAND_UART_TRY_PUBLISH_STALE ||
        fault.latched || fake.disable_count != 1 || fake.write_count != 3) {
        return false;
    }

    return true;
}

int main(void)
{
    if (!test_stale_session()) {
        fputs("stale session was not rejected before MMIO publication\n",
              stderr);
        return 1;
    }
    if (!test_slot_rejection(COMMAND_UART_STATUS_RESPONSE_PENDING,
                             COMMAND_UART_TRY_PUBLISH_BACKPRESSURE)) {
        fputs("occupied response slot was not retained for retry\n", stderr);
        return 1;
    }
    if (!test_slot_rejection(COMMAND_UART_STATUS_RESPONSE_OVERFLOW,
                             COMMAND_UART_TRY_PUBLISH_ERROR)) {
        fputs("response overflow was not rejected before frame writes\n",
              stderr);
        return 1;
    }
    if (!test_frame_commit_and_retry()) {
        fputs("response MMIO frame packing/order or retry failed\n", stderr);
        return 1;
    }
    if (!test_publish_fault_and_session_recovery()) {
        fputs("response publication fault latch/session recovery failed\n",
              stderr);
        return 1;
    }

    puts("UART response MMIO publication, fault latch, and session recovery: PASS");
    return 0;
}
