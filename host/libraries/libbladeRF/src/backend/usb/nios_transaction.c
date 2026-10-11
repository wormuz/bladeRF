#include "nios_transaction.h"
#include "nios_pkt_formats.h"

static bool nios_response_has_zero_tail(const uint8_t *response,
                                        uint8_t first_reserved)
{
    uint8_t i;

    for (i = first_reserved; i < NIOS_PKT_LEN; i++) {
        if (response[i] != 0) {
            return false;
        }
    }
    return true;
}

static bool nios_response_flags_and_reserved_valid(
    const uint8_t *response, uint8_t flags_offset, uint8_t reserved_offset)
{
    return (response[flags_offset] & 0xfcu) == 0 &&
           response[reserved_offset] == 0;
}

/* Standard register packets echo the request identity in their response.
 * A successful flag alone is insufficient: after an ambiguous/late NIOS
 * response it could certify a different request. Keep this check inside the
 * peripheral transaction lock so a mismatch cannot race the next request. */
static bool nios_response_matches_request(const uint8_t *request,
                                          const uint8_t *response)
{
    if (request[0] == NIOS_PKT_LEGACY_MAGIC) {
        uint8_t legacy_device;

        /* Legacy peripheral failures are valid completed responses with a
         * distinct marker; nios_legacy_access maps that marker to FPGA_OP.
         * The configuration byte is echoed and identifies direction,
         * peripheral, and pair count. */
        if ((response[0] != NIOS_PKT_LEGACY_MAGIC &&
             response[0] != NIOS_PKT_LEGACY_ERROR_MAGIC) ||
            response[1] != request[1]) {
            return false;
        }

        legacy_device = request[1] & NIOS_PKT_LEGACY_MODE_DEV_MASK;
        if (legacy_device == NIOS_PKT_LEGACY_DEV_LMS ||
            legacy_device == NIOS_PKT_LEGACY_DEV_SI5338) {
            /* These handlers echo the first register address for both reads
             * and writes. Config-PIO replies normalize addresses to offsets,
             * so their payload cannot be compared this way. */
            return response[2] == request[2];
        }

        return true;
    }

    if (request[0] != response[0]) {
        return false;
    }

    switch (request[0]) {
    case NIOS_PKT_8x8_MAGIC: {
        if (!nios_response_flags_and_reserved_valid(
                response, NIOS_PKT_8x8_IDX_FLAGS, NIOS_PKT_8x8_IDX_RESV1) ||
            !nios_response_has_zero_tail(
                response, NIOS_PKT_8x8_IDX_RESV2)) {
            return false;
        }
        uint8_t req_target, resp_target, req_addr, resp_addr;
        uint8_t req_data, resp_data;
        bool req_write, resp_write;
        nios_pkt_8x8_unpack(request, &req_target, &req_write, &req_addr,
                            &req_data);
        nios_pkt_8x8_unpack(response, &resp_target, &resp_write, &resp_addr,
                            &resp_data);
        return req_target == resp_target && req_write == resp_write &&
               req_addr == resp_addr &&
               (!req_write || req_data == resp_data);
    }
    case NIOS_PKT_8x16_MAGIC: {
        if (!nios_response_flags_and_reserved_valid(
                response, NIOS_PKT_8x16_IDX_FLAGS, NIOS_PKT_8x16_IDX_RESV1) ||
            !nios_response_has_zero_tail(
                response, NIOS_PKT_8x16_IDX_RESV2)) {
            return false;
        }
        uint8_t req_target, resp_target, req_addr, resp_addr;
        uint16_t req_data, resp_data;
        bool req_write, resp_write;
        nios_pkt_8x16_unpack(request, &req_target, &req_write, &req_addr,
                             &req_data);
        nios_pkt_8x16_unpack(response, &resp_target, &resp_write, &resp_addr,
                             &resp_data);
        return req_target == resp_target && req_write == resp_write &&
               req_addr == resp_addr &&
               (!req_write || req_data == resp_data);
    }
    case NIOS_PKT_8x32_MAGIC: {
        if (!nios_response_flags_and_reserved_valid(
                response, NIOS_PKT_8x32_IDX_FLAGS, NIOS_PKT_8x32_IDX_RESV1) ||
            !nios_response_has_zero_tail(
                response, NIOS_PKT_8x32_IDX_RESV2)) {
            return false;
        }
        uint8_t req_target, resp_target, req_addr, resp_addr;
        uint32_t req_data, resp_data;
        bool req_write, resp_write;
        nios_pkt_8x32_unpack(request, &req_target, &req_write, &req_addr,
                             &req_data);
        nios_pkt_8x32_unpack(response, &resp_target, &resp_write, &resp_addr,
                             &resp_data);
        return req_target == resp_target && req_write == resp_write &&
               req_addr == resp_addr &&
               (!req_write || req_data == resp_data);
    }
    case NIOS_PKT_8x64_MAGIC: {
        if (!nios_response_flags_and_reserved_valid(
                response, NIOS_PKT_8x64_IDX_FLAGS, NIOS_PKT_8x64_IDX_RESV1) ||
            !nios_response_has_zero_tail(
                response, NIOS_PKT_8x64_IDX_RESV2)) {
            return false;
        }
        uint8_t req_target, resp_target, req_addr, resp_addr;
        uint64_t req_data, resp_data;
        bool req_write, resp_write;
        nios_pkt_8x64_unpack(request, &req_target, &req_write, &req_addr,
                             &req_data);
        nios_pkt_8x64_unpack(response, &resp_target, &resp_write, &resp_addr,
                             &resp_data);
        return req_target == resp_target && req_write == resp_write &&
               req_addr == resp_addr &&
               (!req_write || req_data == resp_data);
    }
    case NIOS_PKT_16x64_MAGIC: {
        uint8_t req_target, resp_target;
        uint16_t req_addr, resp_addr;
        uint64_t req_data, resp_data;
        bool req_write, resp_write;

        /* These fields are reserved by the packet ABI and are always zero in
         * firmware-generated responses. Reject malformed or stale framing
         * before trusting the echoed request identity. */
        if (!nios_response_flags_and_reserved_valid(
                response, NIOS_PKT_16x64_IDX_FLAGS,
                NIOS_PKT_16x64_IDX_RESV1) ||
            !nios_response_has_zero_tail(
                response, NIOS_PKT_16x64_IDX_RESV2)) {
            return false;
        }

        nios_pkt_16x64_unpack(request, &req_target, &req_write, &req_addr,
                              &req_data);
        nios_pkt_16x64_unpack(response, &resp_target, &resp_write, &resp_addr,
                              &resp_data);
        if (req_target != resp_target || req_write != resp_write ||
            req_addr != resp_addr) {
            return false;
        }

        /* Standard register and RFIC writes echo the complete payload. The
         * batch and atomic-update commands return completion/result data in
         * that field, so their payloads are validated by their callers. */
        if (req_write &&
            (req_target == NIOS_PKT_16x64_TARGET_AD9361 ||
             req_target == NIOS_PKT_16x64_TARGET_RFIC ||
             req_target == NIOS_PKT_16x64_TARGET_GAIN_TABLE_ROW)) {
            return req_data == resp_data;
        }

        return true;
    }
    case NIOS_PKT_32x32_MAGIC: {
        if (!nios_response_flags_and_reserved_valid(
                response, NIOS_PKT_32x32_IDX_FLAGS, NIOS_PKT_32x32_IDX_RESV1) ||
            !nios_response_has_zero_tail(
                response, NIOS_PKT_32x32_IDX_RESV2)) {
            return false;
        }
        uint8_t req_target, resp_target;
        uint32_t req_addr, resp_addr;
        uint32_t req_data, resp_data;
        bool req_write, resp_write;
        nios_pkt_32x32_unpack(request, &req_target, &req_write, &req_addr,
                              &req_data);
        nios_pkt_32x32_unpack(response, &resp_target, &resp_write, &resp_addr,
                              &resp_data);
        return req_target == resp_target && req_write == resp_write &&
               req_addr == resp_addr &&
               (!req_write || req_data == resp_data);
    }
    default:
        /* Legacy and fastlock packets have distinct response contracts and
         * are validated by their format-specific callers. */
        return true;
    }
}

int nios_usb_transaction(struct bladerf_usb *usb, void *buffer,
                         uint32_t buffer_len, uint32_t response_timeout_ms,
                         nios_transaction_out_complete_fn out_complete,
                         void *context,
                         enum nios_transaction_stage *stage)
{
    int status;
    uint8_t request[NIOS_PKT_LEN];
    bool validate_echo = buffer_len == sizeof(request);

    if (validate_echo) {
        memcpy(request, buffer, sizeof(request));
    }

    MUTEX_LOCK(&usb->peripheral_lock);
    if (usb->peripheral_desynchronized) {
        if (stage != NULL) {
            *stage = NIOS_TRANSACTION_DESYNCHRONIZED;
        }
        MUTEX_UNLOCK(&usb->peripheral_lock);
        return BLADERF_ERR_UNEXPECTED;
    }

    status = usb->fn->bulk_transfer(usb->driver, PERIPHERAL_EP_OUT, buffer,
                                    buffer_len, PERIPHERAL_TIMEOUT_MS);
    if (status != 0) {
        usb->peripheral_desynchronized = true;
        if (stage != NULL) {
            *stage = NIOS_TRANSACTION_OUT_FAILED;
        }
        MUTEX_UNLOCK(&usb->peripheral_lock);
        return status;
    }

    if (out_complete != NULL) {
        out_complete(context);
    }

    status = usb->fn->bulk_transfer(usb->driver, PERIPHERAL_EP_IN, buffer,
                                    buffer_len, response_timeout_ms);
    if (status != 0) {
        usb->peripheral_desynchronized = true;
        if (stage != NULL) {
            *stage = NIOS_TRANSACTION_IN_FAILED;
        }
    } else if (validate_echo &&
               !nios_response_matches_request(request, buffer)) {
        usb->peripheral_desynchronized = true;
        status = BLADERF_ERR_UNEXPECTED;
        if (stage != NULL) {
            *stage = NIOS_TRANSACTION_RESPONSE_MISMATCH;
        }
    } else if (stage != NULL) {
        *stage = NIOS_TRANSACTION_COMPLETE;
    }

    MUTEX_UNLOCK(&usb->peripheral_lock);
    return status;
}
