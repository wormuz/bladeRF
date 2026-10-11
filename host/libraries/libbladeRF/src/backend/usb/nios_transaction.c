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

struct nios_legacy_config_range {
    uint8_t start;
    uint8_t length;
};

static bool nios_legacy_config_response_matches(const uint8_t *request,
                                               const uint8_t *response)
{
    static const struct nios_legacy_config_range ranges[] = {
        { NIOS_PKT_LEGACY_PIO_ADDR_CONTROL,
          NIOS_PKT_LEGACY_PIO_LEN_CONTROL },
        { NIOS_PKT_LEGACY_PIO_ADDR_IQ_RX_GAIN,
          NIOS_PKT_LEGACY_PIO_LEN_IQ_RX_GAIN },
        { NIOS_PKT_LEGACY_PIO_ADDR_IQ_RX_PHASE,
          NIOS_PKT_LEGACY_PIO_LEN_IQ_RX_PHASE },
        { NIOS_PKT_LEGACY_PIO_ADDR_IQ_TX_GAIN,
          NIOS_PKT_LEGACY_PIO_LEN_IQ_TX_GAIN },
        { NIOS_PKT_LEGACY_PIO_ADDR_IQ_TX_PHASE,
          NIOS_PKT_LEGACY_PIO_LEN_IQ_TX_PHASE },
        { NIOS_PKT_LEGACY_PIO_ADDR_FPGA_VERSION,
          NIOS_PKT_LEGACY_PIO_LEN_FPGA_VERSION },
        { NIOS_PKT_LEGACY_PIO_ADDR_RX_TIMESTAMP,
          NIOS_PKT_LEGACY_PIO_LEN_RX_TIMESTAMP },
        { NIOS_PKT_LEGACY_PIO_ADDR_TX_TIMESTAMP,
          NIOS_PKT_LEGACY_PIO_LEN_TX_TIMESTAMP },
        { NIOS_PKT_LEGACY_PIO_ADDR_VCTCXO,
          NIOS_PKT_LEGACY_PIO_LEN_VCTCXO },
        { NIOS_PKT_LEGACY_PIO_ADDR_XB200_SYNTH,
          NIOS_PKT_LEGACY_PIO_LEN_XB200_SYNTH },
        { NIOS_PKT_LEGACY_PIO_ADDR_EXP,
          NIOS_PKT_LEGACY_PIO_LEN_EXP },
        { NIOS_PKT_LEGACY_PIO_ADDR_EXP_DIR,
          NIOS_PKT_LEGACY_PIO_LEN_EXP_DIR },
    };
    const uint8_t count = request[1] & NIOS_PKT_LEGACY_MODE_CNT_MASK;
    const uint8_t first_addr = request[2];
    uint8_t i, range_index;
    uint8_t range_start = 0;
    uint8_t range_length = 0;

    if (count == 0) {
        return false;
    }

    for (range_index = 0;
         range_index < sizeof(ranges) / sizeof(ranges[0]);
         range_index++) {
        if (first_addr >= ranges[range_index].start &&
            first_addr < (uint8_t)(ranges[range_index].start +
                                   ranges[range_index].length)) {
            range_start = ranges[range_index].start;
            range_length = ranges[range_index].length;
            break;
        }
    }

    if (range_length == 0) {
        return false;
    }

    /* Legacy firmware stops once the current PIO value has been exhausted,
     * even when a larger pair count was requested. Validate only the tuples
     * that it actually emits. */
    if (count > (uint8_t)(range_length - (first_addr - range_start))) {
        i = (uint8_t)(range_length - (first_addr - range_start));
    } else {
        i = count;
    }

    for (range_index = 0; range_index < i; range_index++) {
        const uint8_t request_offset = (uint8_t)(2 + 2 * range_index);
        const uint8_t expected_response_addr =
            (uint8_t)(request[request_offset] - range_start);
        if (response[request_offset] != expected_response_addr) {
            return false;
        }
    }

    return true;
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

        /* Device-reported failures carry a valid config echo, but their
         * payload is not meaningful and may contain stale response bytes. */
        if (response[0] == NIOS_PKT_LEGACY_ERROR_MAGIC) {
            return true;
        }

        legacy_device = request[1] & NIOS_PKT_LEGACY_MODE_DEV_MASK;
        if (legacy_device == NIOS_PKT_LEGACY_DEV_CONFIG) {
            return nios_legacy_config_response_matches(request, response);
        }

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
    case NIOS_PKT_RETUNE_MAGIC:
        /* Retune responses define only three flag bits and zero the tail. */
        return (response[NIOS_PKT_RETUNERESP_IDX_FLAGS] & 0xf8u) == 0 &&
               nios_response_has_zero_tail(
                   response, NIOS_PKT_RETUNERESP_IDX_RESV);

    case NIOS_PKT_RETUNE2_MAGIC:
        /* Retune2 responses define four flag bits and zero the tail. */
        return (response[NIOS_PKT_RETUNE2_RESP_IDX_FLAGS] & 0xf0u) == 0 &&
               nios_response_has_zero_tail(
                   response, NIOS_PKT_RETUNE2_RESP_IDX_RESV);

    default:
        /* Legacy packets have distinct, variable-length response contracts
         * and are validated by their format-specific callers. */
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
