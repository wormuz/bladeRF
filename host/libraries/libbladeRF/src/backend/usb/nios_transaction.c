#include "nios_transaction.h"
#include "nios_pkt_formats.h"

/* Standard register packets echo the request identity in their response.
 * A successful flag alone is insufficient: after an ambiguous/late NIOS
 * response it could certify a different request. Keep this check inside the
 * peripheral transaction lock so a mismatch cannot race the next request. */
static bool nios_response_matches_request(const uint8_t *request,
                                          const uint8_t *response)
{
    if (request[0] == NIOS_PKT_LEGACY_MAGIC) {
        /* Legacy peripheral failures are valid completed responses with a
         * distinct marker; nios_legacy_access maps that marker to FPGA_OP.
         * The configuration byte is echoed and identifies direction,
         * peripheral, and pair count. */
        return (response[0] == NIOS_PKT_LEGACY_MAGIC ||
                response[0] == NIOS_PKT_LEGACY_ERROR_MAGIC) &&
               response[1] == request[1];
    }

    if (request[0] != response[0]) {
        return false;
    }

    switch (request[0]) {
    case NIOS_PKT_8x8_MAGIC: {
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
        bool req_write, resp_write;
        nios_pkt_16x64_unpack(request, &req_target, &req_write, &req_addr,
                              NULL);
        nios_pkt_16x64_unpack(response, &resp_target, &resp_write, &resp_addr,
                              NULL);
        return req_target == resp_target && req_write == resp_write &&
               req_addr == resp_addr;
    }
    case NIOS_PKT_32x32_MAGIC: {
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
