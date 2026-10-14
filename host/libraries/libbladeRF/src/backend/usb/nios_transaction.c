#include "nios_transaction.h"
#include "nios_pkt_formats.h"
#include "helpers/timeout.h"

static uint64_t nios_read_le64(const uint8_t *buf, uint8_t offset)
{
    uint64_t value = 0;
    uint8_t i;

    for (i = 0; i < 8; i++) {
        value |= (uint64_t)buf[offset + i] << (8 * i);
    }
    return value;
}

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
                                          const uint8_t *response,
                                          bool session_generation_valid,
                                          uint32_t session_generation)
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

        nios_pkt_16x64_unpack(request, &req_target, &req_write, &req_addr,
                              &req_data);
        nios_pkt_16x64_unpack(response, &resp_target, &resp_write, &resp_addr,
                              &resp_data);
        /* BUSY is a response-only extension for tagged RFIC writes. All
         * other flag bits and the reserved header byte remain zero. */
        if ((response[NIOS_PKT_16x64_IDX_FLAGS] & 0xf8u) != 0 ||
            response[NIOS_PKT_16x64_IDX_RESV1] != 0) {
            return false;
        }
        if (req_target == NIOS_PKT_16x64_TARGET_RFIC && req_write) {
            if (nios_pkt_16x64_transaction_id(request) !=
                nios_pkt_16x64_transaction_id(response)) {
                return false;
            }
        } else if (nios_pkt_16x64_transaction_id(request) != 0 ||
                   !nios_response_has_zero_tail(
                       response, NIOS_PKT_16x64_IDX_RESV2)) {
            return false;
        }
        if (req_target != resp_target || req_write != resp_write ||
            req_addr != resp_addr) {
            return false;
        }
        if (nios_pkt_16x64_resp_is_busy(response) &&
            (req_target != NIOS_PKT_16x64_TARGET_RFIC || !req_write ||
             nios_pkt_16x64_transaction_id(request) == 0 ||
             (response[NIOS_PKT_16x64_IDX_FLAGS] &
              NIOS_PKT_16x64_FLAG_SUCCESS) != 0)) {
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
    case NIOS_PKT_RETUNE_MAGIC: {
        const uint8_t flags = response[NIOS_PKT_RETUNERESP_IDX_FLAGS];
        const uint64_t timestamp =
            nios_read_le64(request, NIOS_PKT_RETUNE_IDX_TIME);

        /* Valid tune time is NOW-only. Cancel failed/pending flags are valid
         * only on a queue-clear request and cannot accompany success. */
        if ((flags & 0xe0u) != 0 ||
            !nios_response_has_zero_tail(
                response, NIOS_PKT_RETUNERESP_IDX_RESV) ||
            ((flags & NIOS_PKT_RETUNERESP_FLAG_BUSY) != 0 &&
             (flags & (NIOS_PKT_RETUNERESP_FLAG_SUCCESS |
                       NIOS_PKT_RETUNERESP_FLAG_CANCEL_FAILED |
                       NIOS_PKT_RETUNERESP_FLAG_CANCEL_PENDING)) != 0) ||
            ((flags & NIOS_PKT_RETUNERESP_FLAG_TSVTUNE_VALID) != 0 &&
             timestamp != NIOS_PKT_RETUNE_NOW) ||
            ((flags & NIOS_PKT_RETUNERESP_FLAG_CANCEL_FAILED) != 0 &&
             (timestamp != NIOS_PKT_RETUNE_CLEAR_QUEUE ||
              (flags & NIOS_PKT_RETUNERESP_FLAG_SUCCESS) != 0)) ||
            ((flags & NIOS_PKT_RETUNERESP_FLAG_CANCEL_PENDING) != 0 &&
             (timestamp != NIOS_PKT_RETUNE_CLEAR_QUEUE ||
              (flags & (NIOS_PKT_RETUNERESP_FLAG_SUCCESS |
                        NIOS_PKT_RETUNERESP_FLAG_CANCEL_FAILED)) != 0))) {
            return false;
        }
        return true;
    }

    case NIOS_PKT_RETUNE2_MAGIC: {
        const uint8_t flags = response[NIOS_PKT_RETUNE2_RESP_IDX_FLAGS];
        const uint64_t timestamp =
            nios_read_le64(request, NIOS_PKT_RETUNE2_IDX_TIME);
        const uint16_t request_token =
            nios_pkt_retune2_unpack_request_token(request);
        const uint32_t response_session_generation =
            (uint32_t)response[NIOS_PKT_RETUNE2_RESP_IDX_SESSION_GENERATION] |
            ((uint32_t)response[NIOS_PKT_RETUNE2_RESP_IDX_SESSION_GENERATION + 1]
             << 8) |
            ((uint32_t)response[NIOS_PKT_RETUNE2_RESP_IDX_SESSION_GENERATION + 2]
             << 16) |
            ((uint32_t)response[NIOS_PKT_RETUNE2_RESP_IDX_SESSION_GENERATION + 3]
             << 24);

        /* Retune2's invalid-time bit only accompanies a failed scheduled
         * request. Async-pending is an acceptance ACK for tokened NOW only;
         * terminal outcome comes from the correlated completion record. */
        if ((flags & 0x80u) != 0 ||
            ((flags & NIOS_PKT_RETUNE2_RESP_FLAG_ASYNC_PENDING) != 0 &&
             (timestamp != NIOS_PKT_RETUNE2_NOW || request_token == 0 ||
              (flags & NIOS_PKT_RETUNE2_RESP_FLAG_SUCCESS) == 0 ||
              (flags & (NIOS_PKT_RETUNE2_RESP_FLAG_TSVTUNE_VALID |
                        NIOS_PKT_RETUNE2_RESP_FLAG_INVALID_TIMESTAMP |
                        NIOS_PKT_RETUNE2_RESP_FLAG_CANCEL_FAILED |
                        NIOS_PKT_RETUNE2_RESP_FLAG_CANCEL_PENDING |
                        NIOS_PKT_RETUNE2_RESP_FLAG_BUSY)) != 0 ||
              !nios_response_has_zero_tail(
                  response, NIOS_PKT_RETUNE2_RESP_IDX_ASYNC_RESV))) ||
            (request_token == 0 &&
             !nios_response_has_zero_tail(
                 response, NIOS_PKT_RETUNE2_RESP_IDX_RESV)) ||
            (request_token != 0 &&
             ((uint16_t)response[NIOS_PKT_RETUNE2_RESP_IDX_TOKEN] |
              ((uint16_t)response[NIOS_PKT_RETUNE2_RESP_IDX_TOKEN + 1] << 8)) !=
                 request_token) ||
            (request_token != 0 &&
             (!session_generation_valid ||
              response_session_generation != session_generation)) ||
            ((flags & NIOS_PKT_RETUNE2_RESP_FLAG_BUSY) != 0 &&
             (flags & (NIOS_PKT_RETUNE2_RESP_FLAG_SUCCESS |
                       NIOS_PKT_RETUNE2_RESP_FLAG_INVALID_TIMESTAMP |
                       NIOS_PKT_RETUNE2_RESP_FLAG_CANCEL_FAILED |
                       NIOS_PKT_RETUNE2_RESP_FLAG_CANCEL_PENDING |
                       NIOS_PKT_RETUNE2_RESP_FLAG_ASYNC_PENDING)) != 0) ||
            ((flags & NIOS_PKT_RETUNE2_RESP_FLAG_TSVTUNE_VALID) != 0 &&
             timestamp != NIOS_PKT_RETUNE2_NOW) ||
            ((flags & NIOS_PKT_RETUNE2_RESP_FLAG_CANCEL_FAILED) != 0 &&
             (timestamp != NIOS_PKT_RETUNE2_CLEAR_QUEUE ||
              (flags & NIOS_PKT_RETUNE2_RESP_FLAG_SUCCESS) != 0)) ||
            ((flags & NIOS_PKT_RETUNE2_RESP_FLAG_CANCEL_PENDING) != 0 &&
             (timestamp != NIOS_PKT_RETUNE2_CLEAR_QUEUE ||
              (flags & (NIOS_PKT_RETUNE2_RESP_FLAG_SUCCESS |
                        NIOS_PKT_RETUNE2_RESP_FLAG_CANCEL_FAILED |
                        NIOS_PKT_RETUNE2_RESP_FLAG_ASYNC_PENDING)) != 0)) ||
            ((flags & NIOS_PKT_RETUNE2_RESP_FLAG_INVALID_TIMESTAMP) != 0 &&
             (timestamp == NIOS_PKT_RETUNE2_NOW ||
              timestamp == NIOS_PKT_RETUNE2_CLEAR_QUEUE ||
              (flags & (NIOS_PKT_RETUNE2_RESP_FLAG_SUCCESS |
                        NIOS_PKT_RETUNE2_RESP_FLAG_CANCEL_FAILED |
                        NIOS_PKT_RETUNE2_RESP_FLAG_CANCEL_PENDING |
                        NIOS_PKT_RETUNE2_RESP_FLAG_ASYNC_PENDING)) != 0))) {
            return false;
        }
        return true;
    }

    default:
        /* Legacy packets have distinct, variable-length response contracts
         * and are validated by their format-specific callers. */
        return true;
    }
}

static bool nios_is_tagged_rfic_write(const uint8_t *packet)
{
    return packet[NIOS_PKT_16x64_IDX_MAGIC] == NIOS_PKT_16x64_MAGIC &&
           packet[NIOS_PKT_16x64_IDX_TARGET_ID] ==
               NIOS_PKT_16x64_TARGET_RFIC &&
           (packet[NIOS_PKT_16x64_IDX_FLAGS] &
            NIOS_PKT_16x64_FLAG_WRITE) != 0 &&
           nios_pkt_16x64_transaction_id(packet) != 0;
}

static bool nios_is_recovery_status_read(const uint8_t *packet)
{
    uint16_t address;

    if (packet[NIOS_PKT_16x64_IDX_MAGIC] != NIOS_PKT_16x64_MAGIC ||
        packet[NIOS_PKT_16x64_IDX_TARGET_ID] !=
            NIOS_PKT_16x64_TARGET_RFIC ||
        (packet[NIOS_PKT_16x64_IDX_FLAGS] &
         NIOS_PKT_16x64_FLAG_WRITE) != 0) {
        return false;
    }

    address = (uint16_t)packet[NIOS_PKT_16x64_IDX_ADDR] |
              ((uint16_t)packet[NIOS_PKT_16x64_IDX_ADDR + 1] << 8);
    return address == 0x0f00u || address == 0x0f0cu;
}

/* Only fixed-width request formats with a format-specific response identity
 * can be retained for ordered recovery. Legacy and scheduled retune packets
 * have separate response lifecycles. */
static bool nios_is_fixed_read_request(const uint8_t *packet)
{
    uint8_t target, addr8;
    uint8_t data8;
    uint16_t addr16;
    uint16_t data16;
    uint32_t data32;
    uint32_t addr32;
    uint64_t data64;
    bool write;

    switch (packet[0]) {
    case NIOS_PKT_8x8_MAGIC:
        nios_pkt_8x8_unpack(packet, &target, &write, &addr8, &data8);
        return !write;
    case NIOS_PKT_8x16_MAGIC:
        nios_pkt_8x16_unpack(packet, &target, &write, &addr8, &data16);
        return !write;
    case NIOS_PKT_8x32_MAGIC:
        nios_pkt_8x32_unpack(packet, &target, &write, &addr8, &data32);
        return !write;
    case NIOS_PKT_8x64_MAGIC:
        nios_pkt_8x64_unpack(packet, &target, &write, &addr8, &data64);
        return !write;
    case NIOS_PKT_16x64_MAGIC:
        nios_pkt_16x64_unpack(packet, &target, &write, &addr16, &data64);
        return !write && nios_pkt_16x64_transaction_id(packet) == 0;
    case NIOS_PKT_32x32_MAGIC: {
        nios_pkt_32x32_unpack(packet, &target, &write, &addr32, &data32);
        return !write;
    }
    default:
        return false;
    }
}

/* A NIOS read may have executed even when its IN response did not arrive.
 * Do not replay it: some reads have small side effects (for example enabling
 * the TX monitor). Send a structurally distinct, side-effect-free 8x32 read
 * as an ordered barrier, consume a late original response if present, then
 * accept the barrier response. A zero-byte timeout leaves this identity pending;
 * partial frames and mismatches remain fatal because framing is ambiguous. */
static int nios_drain_pending_read_locked(
    struct bladerf_usb *usb, struct usb_peripheral_state *state,
    uint32_t timeout_ms, enum nios_transaction_stage *stage)
{
    uint8_t barrier[NIOS_PKT_LEN] = {0};
    uint8_t response[NIOS_PKT_LEN];
    uint64_t started_ns = 0;
    bool pending_matches, pending_write_matches;
    bool pending_retune2_matches, barrier_matches;
    int status;

    nios_pkt_8x32_pack(barrier, NIOS_PKT_8x32_TARGET_VERSION, false,
                       0, 0);
    /* The barrier identity must not alias the retained read, even when that
     * read used the same packet format and target. CONTROL is also a safe
     * read-only target and has no read side effect. */
    if (state->pending_read_request[NIOS_PKT_8x32_IDX_MAGIC] ==
            NIOS_PKT_8x32_MAGIC &&
        state->pending_read_request[NIOS_PKT_8x32_IDX_TARGET_ID] ==
            NIOS_PKT_8x32_TARGET_VERSION &&
        state->pending_read_request[NIOS_PKT_8x32_IDX_ADDR] == 0) {
        nios_pkt_8x32_pack(barrier, NIOS_PKT_8x32_TARGET_CONTROL, false,
                           0, 0);
    }

    if (bladerf_monotonic_now_ns(&started_ns) != 0) {
        state->desynchronized = true;
        status = BLADERF_ERR_UNEXPECTED;
        if (stage != NULL) {
            *stage = NIOS_TRANSACTION_IN_FAILED;
        }
        return status;
    }

    status = usb->fn->bulk_transfer(usb->driver, PERIPHERAL_EP_OUT,
                                    barrier, sizeof(barrier),
                                    PERIPHERAL_TIMEOUT_MS, NULL, NULL);
    if (status != 0) {
        state->desynchronized = true;
        if (stage != NULL) {
            *stage = NIOS_TRANSACTION_OUT_FAILED;
        }
        return status;
    }

    for (;;) {
        unsigned int remaining_ms = 0;
        uint32_t transferred = 0;
        bool transferred_valid = false;
        int deadline_status = bladerf_timeout_remaining_ms(
            started_ns, timeout_ms, &remaining_ms);

        if ((deadline_status != 0 && deadline_status != BLADERF_ERR_TIMEOUT) ||
            deadline_status == BLADERF_ERR_TIMEOUT || remaining_ms == 0) {
            if (deadline_status != BLADERF_ERR_TIMEOUT &&
                deadline_status != 0) {
                state->desynchronized = true;
                status = BLADERF_ERR_UNEXPECTED;
            } else {
                /* This barrier itself may now have a late reply. Without
                 * retaining its identity, another recovery attempt could
                 * leave that reply queued behind the next barrier. */
                state->desynchronized = true;
                status = BLADERF_ERR_TIMEOUT;
            }
            if (stage != NULL) {
                *stage = NIOS_TRANSACTION_IN_FAILED;
            }
            return status;
        }

        status = usb->fn->bulk_transfer(
            usb->driver, PERIPHERAL_EP_IN, response, sizeof(response),
            remaining_ms, &transferred, &transferred_valid);
        if (!transferred_valid || transferred > sizeof(response)) {
            state->desynchronized = true;
            if (stage != NULL) {
                *stage = NIOS_TRANSACTION_IN_FAILED;
            }
            return BLADERF_ERR_UNEXPECTED;
        }
        if (transferred != sizeof(response)) {
            if (status == BLADERF_ERR_TIMEOUT && transferred == 0) {
                state->desynchronized = true;
                if (stage != NULL) {
                    *stage = NIOS_TRANSACTION_IN_FAILED;
                }
                return BLADERF_ERR_TIMEOUT;
            }
            state->desynchronized = true;
            if (stage != NULL) {
                *stage = NIOS_TRANSACTION_IN_FAILED;
            }
            return status != 0 ? status : BLADERF_ERR_IO;
        }
        if (status != 0 && status != BLADERF_ERR_TIMEOUT) {
            state->desynchronized = true;
            if (stage != NULL) {
                *stage = NIOS_TRANSACTION_IN_FAILED;
            }
            return status;
        }

        pending_matches = nios_response_matches_request(
            state->pending_read_request, response,
            state->command_session_generation_valid,
            state->command_session_generation);
        pending_write_matches = state->pending_rfic_write &&
            nios_response_matches_request(
                state->pending_rfic_request, response,
                state->command_session_generation_valid,
                state->command_session_generation);
        pending_retune2_matches = state->pending_retune2_response &&
            nios_response_matches_request(
                state->pending_retune2_request, response, true,
                state->pending_retune2_session_generation);
        barrier_matches = nios_response_matches_request(
            barrier, response, state->command_session_generation_valid,
            state->command_session_generation);
        if ((pending_matches && !barrier_matches) ||
            pending_write_matches || pending_retune2_matches) {
            /* Another request class may already have an ambiguous reply in
             * flight. Consume those older ordered frames too, but retain its
             * transaction identity until its own status/queue-clear barrier. */
            continue;
        }
        if (barrier_matches && !pending_matches) {
            state->pending_read_response = false;
            memset(state->pending_read_request, 0,
                   sizeof(state->pending_read_request));
            if (stage != NULL) {
                *stage = NIOS_TRANSACTION_COMPLETE;
            }
            return 0;
        }

        state->desynchronized = true;
        if (stage != NULL) {
            *stage = NIOS_TRANSACTION_RESPONSE_MISMATCH;
        }
        return BLADERF_ERR_UNEXPECTED;
    }
}

static bool nios_is_retune2_recovery_request(const uint8_t *packet)
{
    uint8_t target;
    uint8_t addr;
    uint32_t data;
    bool write;

    if (packet[NIOS_PKT_8x32_IDX_MAGIC] == NIOS_PKT_8x32_MAGIC) {
        nios_pkt_8x32_unpack(packet, &target, &write, &addr, &data);
        if (!write) {
            return true;
        }
        return target == NIOS_PKT_8x32_TARGET_RETUNE2_COMPLETION &&
               addr <= NIOS_PKT_8x32_RETUNE2_FAILURE_SELECT_TX;
    }
    if (nios_is_recovery_status_read(packet)) {
        return true;
    }
    if (packet[NIOS_PKT_RETUNE2_IDX_MAGIC] == NIOS_PKT_RETUNE2_MAGIC) {
        return nios_read_le64(packet, NIOS_PKT_RETUNE2_IDX_TIME) ==
               NIOS_PKT_RETUNE2_CLEAR_QUEUE;
    }
    return false;
}

int nios_usb_transaction(struct bladerf_usb *usb, void *buffer,
                         uint32_t buffer_len, uint32_t response_timeout_ms,
                         nios_transaction_out_complete_fn out_complete,
                         void *context,
                         enum nios_transaction_stage *stage)
{
    struct usb_peripheral_state *state = usb->peripheral;
    int status;
    uint8_t request[NIOS_PKT_LEN];
    bool validate_echo = buffer_len == sizeof(request);
    bool recovering_pending_write = false;
    bool recovering_pending_retune2 = false;
    uint64_t response_started_ns = 0;
    uint32_t response_bytes = 0;

    if (validate_echo) {
        memcpy(request, buffer, sizeof(request));
    }

    MUTEX_LOCK(&state->lock);
    if (state->desynchronized) {
        if (stage != NULL) {
            *stage = NIOS_TRANSACTION_DESYNCHRONIZED;
        }
        MUTEX_UNLOCK(&state->lock);
        return BLADERF_ERR_UNEXPECTED;
    }

    if (state->pending_read_response) {
        status = nios_drain_pending_read_locked(
            usb, state, response_timeout_ms, stage);
        if (status != 0) {
            MUTEX_UNLOCK(&state->lock);
            return status;
        }
    }

    if (state->pending_rfic_write) {
        recovering_pending_write = true;
        /* While one RFIC write may still have a late reply in the endpoint,
         * allow only the two status reads used to resolve it or an exact
         * retry of that same tagged write. */
        if (!validate_echo ||
            (!nios_is_recovery_status_read(request) &&
             memcmp(request, state->pending_rfic_request,
                    sizeof(request)) != 0)) {
            if (stage != NULL) {
                *stage = NIOS_TRANSACTION_PENDING_RESPONSE;
            }
            MUTEX_UNLOCK(&state->lock);
            return BLADERF_ERR_WOULD_BLOCK;
        }
    }
    if (state->pending_retune2_response) {
        recovering_pending_retune2 = true;
        if (!validate_echo || !nios_is_retune2_recovery_request(request)) {
            if (stage != NULL) {
                *stage = NIOS_TRANSACTION_PENDING_RESPONSE;
            }
            MUTEX_UNLOCK(&state->lock);
            return BLADERF_ERR_WOULD_BLOCK;
        }
    }

    status = usb->fn->bulk_transfer(usb->driver, PERIPHERAL_EP_OUT, buffer,
                                    buffer_len, PERIPHERAL_TIMEOUT_MS,
                                    NULL, NULL);
    if (status != 0) {
        state->desynchronized = true;
        if (stage != NULL) {
            *stage = NIOS_TRANSACTION_OUT_FAILED;
        }
        MUTEX_UNLOCK(&state->lock);
        return status;
    }

    if (out_complete != NULL) {
        out_complete(context);
    }

    if (bladerf_monotonic_now_ns(&response_started_ns) != 0) {
        state->desynchronized = true;
        status = BLADERF_ERR_UNEXPECTED;
        if (stage != NULL) {
            *stage = NIOS_TRANSACTION_IN_FAILED;
        }
        MUTEX_UNLOCK(&state->lock);
        return status;
    }

    for (;;) {
        unsigned int remaining_ms = 0;
        bool current_matches;
        bool pending_matches = false;
        bool pending_retune2_matches = false;
        bool transferred_valid = false;
        uint32_t transferred = 0;
        uint32_t requested;
        int deadline_status;

        deadline_status = bladerf_timeout_remaining_ms(
            response_started_ns, response_timeout_ms, &remaining_ms);
        if (deadline_status != 0 && deadline_status != BLADERF_ERR_TIMEOUT) {
            state->desynchronized = true;
            status = BLADERF_ERR_UNEXPECTED;
            if (stage != NULL) {
                *stage = NIOS_TRANSACTION_IN_FAILED;
            }
            break;
        } else if (deadline_status == BLADERF_ERR_TIMEOUT ||
                   remaining_ms == 0) {
            status = BLADERF_ERR_TIMEOUT;
        } else {
            requested = buffer_len - response_bytes;
            status = usb->fn->bulk_transfer(
                usb->driver, PERIPHERAL_EP_IN,
                (uint8_t *)buffer + response_bytes, requested,
                remaining_ms, &transferred, &transferred_valid);
            if (!transferred_valid || transferred > requested) {
                state->desynchronized = true;
                status = BLADERF_ERR_UNEXPECTED;
                if (stage != NULL) {
                    *stage = NIOS_TRANSACTION_IN_FAILED;
                }
                break;
            }
            response_bytes += transferred;
        }

        if (response_bytes == buffer_len) {
            if (status != 0 && status != BLADERF_ERR_TIMEOUT) {
                state->desynchronized = true;
                if (stage != NULL) {
                    *stage = NIOS_TRANSACTION_IN_FAILED;
                }
                break;
            }

            current_matches = !validate_echo ||
                nios_response_matches_request(
                    request, buffer,
                    state->command_session_generation_valid,
                    state->command_session_generation);
            if (recovering_pending_write &&
                state->pending_rfic_write) {
                pending_matches = nios_response_matches_request(
                    state->pending_rfic_request, buffer,
                    state->command_session_generation_valid,
                    state->command_session_generation);
            }
            if (recovering_pending_retune2 &&
                state->pending_retune2_response) {
                pending_retune2_matches = nios_response_matches_request(
                    state->pending_retune2_request, buffer, true,
                    state->pending_retune2_session_generation);
            }

            /* A retry of the exact tagged RFIC write can produce two
             * identical ACKs: the original response may still be in the
             * command-UART serializer when NIOS handles the retry and emits
             * its idempotent duplicate ACK. When one response matches both
             * the pending request and the retry currently being issued, it
             * is impossible to tell which copy arrived. Keep the pending
             * identity until the next recovery status response proves that
             * all older ordered responses have been drained. */
            if (pending_retune2_matches) {
                state->pending_retune2_response = false;
                memset(state->pending_retune2_request, 0,
                       sizeof(state->pending_retune2_request));
                state->pending_retune2_session_generation = 0;
            }
            if (current_matches) {
                if (recovering_pending_write &&
                    nios_is_recovery_status_read(request)) {
                    /* UART responses preserve commit order. A matching
                     * status response cannot pass an older duplicate write
                     * ACK, so reaching it resolves the ambiguous retry ACK. */
                    state->pending_rfic_write = false;
                    memset(state->pending_rfic_request, 0,
                           sizeof(state->pending_rfic_request));
                }
                if (validate_echo &&
                    request[NIOS_PKT_8x32_IDX_MAGIC] ==
                        NIOS_PKT_8x32_MAGIC &&
                    (request[NIOS_PKT_8x32_IDX_FLAGS] &
                     NIOS_PKT_8x32_FLAG_WRITE) == 0 &&
                    request[NIOS_PKT_8x32_IDX_TARGET_ID] ==
                        NIOS_PKT_8x32_TARGET_COMMAND_SESSION_GENERATION &&
                    request[NIOS_PKT_8x32_IDX_ADDR] == 0 &&
                    (response[NIOS_PKT_8x32_IDX_FLAGS] &
                     NIOS_PKT_8x32_FLAG_SUCCESS) != 0) {
                    uint32_t value;

                    nios_pkt_8x32_unpack(buffer, NULL, NULL, NULL, &value);
                    state->command_session_generation = value;
                    state->command_session_generation_valid = true;
                }
                if (recovering_pending_retune2 &&
                    state->pending_retune2_response &&
                    request[NIOS_PKT_RETUNE2_IDX_MAGIC] ==
                        NIOS_PKT_RETUNE2_MAGIC &&
                    nios_read_le64(request, NIOS_PKT_RETUNE2_IDX_TIME) ==
                        NIOS_PKT_RETUNE2_CLEAR_QUEUE) {
                    /* An acknowledged queue clear is the terminal recovery
                     * barrier for the scheduled request with the missing ACK. */
                    state->pending_retune2_response = false;
                    memset(state->pending_retune2_request, 0,
                           sizeof(state->pending_retune2_request));
                    state->pending_retune2_session_generation = 0;
                }
                if (stage != NULL) {
                    *stage = NIOS_TRANSACTION_COMPLETE;
                }
                status = 0;
                break;
            }
            if (pending_matches || pending_retune2_matches) {
                /* Consume old ordered responses before assembling this
                 * request's response. Keep a tagged RFIC write identity until
                 * the current status response is the ordering barrier: a
                 * retry may have left multiple byte-identical ACKs queued. */
                response_bytes = 0;
                continue;
            }

            state->desynchronized = true;
            status = BLADERF_ERR_UNEXPECTED;
            if (stage != NULL) {
                *stage = NIOS_TRANSACTION_RESPONSE_MISMATCH;
            }
            break;
        }

        if (status != 0 && status != BLADERF_ERR_TIMEOUT) {
            state->desynchronized = true;
            if (stage != NULL) {
                *stage = NIOS_TRANSACTION_IN_FAILED;
            }
            break;
        }

        if (status == BLADERF_ERR_TIMEOUT) {
            if (response_bytes == 0 && validate_echo &&
                request[NIOS_PKT_RETUNE2_IDX_MAGIC] ==
                    NIOS_PKT_RETUNE2_MAGIC &&
                nios_pkt_retune2_unpack_request_token(request) != 0) {
                if (!state->command_session_generation_valid) {
                    state->desynchronized = true;
                    status = BLADERF_ERR_UNEXPECTED;
                } else {
                    memcpy(state->pending_retune2_request, request,
                           sizeof(request));
                    state->pending_retune2_session_generation =
                        state->command_session_generation;
                    state->pending_retune2_response = true;
                }
            } else if (response_bytes == 0 && validate_echo &&
                nios_is_tagged_rfic_write(request) &&
                (!state->pending_rfic_write ||
                 memcmp(request, state->pending_rfic_request,
                        sizeof(request)) == 0)) {
                if (!state->pending_rfic_write) {
                    memcpy(state->pending_rfic_request, request,
                           sizeof(request));
                    state->pending_rfic_write = true;
                }
            } else if (response_bytes == 0 && validate_echo &&
                       nios_is_fixed_read_request(request)) {
                memcpy(state->pending_read_request, request,
                       sizeof(state->pending_read_request));
                state->pending_read_response = true;
            } else {
                /* A partial response has consumed endpoint bytes and cannot
                 * be reconstructed after the absolute deadline. */
                state->desynchronized = true;
            }
            if (stage != NULL) {
                *stage = NIOS_TRANSACTION_IN_FAILED;
            }
            break;
        }

        if (transferred == 0) {
            state->desynchronized = true;
            status = BLADERF_ERR_IO;
            if (stage != NULL) {
                *stage = NIOS_TRANSACTION_IN_FAILED;
            }
            break;
        }

        /* Successful short packets also contribute to the same fixed frame;
         * request only the remaining bytes on the next iteration. */
    }

    MUTEX_UNLOCK(&state->lock);
    return status;
}
