/*
 * Copyright (c) 2018 Nuand LLC
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#ifndef BLADERF_NIOS_PKT_RETUNE2_H_
#define BLADERF_NIOS_PKT_RETUNE2_H_

/* The retune2 helpers are included directly by host and FPGA translation
 * units as well as through nios_pkt_formats.h. Keep their legacy packet
 * bound available in either include path. */
#ifndef NIOS_PKT_LEN
#define NIOS_PKT_LEN 16
#endif

#ifndef BLADERF_NIOS_BUILD
#   include <libbladeRF.h>
#else
#   include "libbladeRF_nios_compat.h"
#endif

#include <stdint.h>

/* This file defines the Host <-> FPGA (NIOS II) packet formats for
 * retune2 messages. This packet is formatted, as follows. All values are
 * little-endian.
 *
 *                              Request
 *                      ----------------------
 *
 * +================+=========================================================+
 * |  Byte offset   |                       Description                       |
 * +================+=========================================================+
 * |        0       | Magic Value                                             |
 * +----------------+---------------------------------------------------------+
 * |        1       | 64-bit timestamp denoting when to retune. (Note 1)      |
 * +----------------+---------------------------------------------------------+
 * |        9       | 16-bit Nios fast lock profile number to load (Note 2)   |
 * +----------------+---------------------------------------------------------+
 * |       11       | 8-bit RFFE fast lock profile slot to use                |
 * +----------------+---------------------------------------------------------+
 * |       12       | Bit  7:     RX bit (set if this is an RX profile        |
 * |                | Bits 6:     TX output port selection                    |
 * |                | Bits [5:0]: RX input port selection                     |
 * +----------------+---------------------------------------------------------+
 * |       13       | Bits [7:6]: External TX2 SPDT switch setting            |
 * |                | Bits [5:4]: External TX1 SPDT switch setting            |
 * |                | Bits [3:2]: External RX2 SPDT switch setting            |
 * |                | Bits [1:0]: External RX1 SPDT switch setting            |
 * +----------------+---------------------------------------------------------+
 * |       14-15    | 16-bit host request token (zero on legacy protocol).    |
 * +----------------+---------------------------------------------------------+
 *
 * (Note 1) Special Timestamp Values:
 *
 * Tune "Now":          0x0000000000000000
 * Clear Retune Queue:  0xffffffffffffffff
 *
 * When the "Clear Retune Queue" value is used, all of the other tuning
 * parameters are ignored.
 *
 * (Note 2) Packed as follows:
 *
 * +================+=======================+
 * |   Byte offset  | (MSB)   Value    (LSB)|
 * +================+=======================+
 * |       0        |  NIOS_PROFILE[7:0]    |
 * +----------------+-----------------------+
 * |       1        |  NIOS_PROFILE[15:8]   |
 * +----------------+-----------------------+
 *
 */

#define NIOS_PKT_RETUNE2_IDX_MAGIC        0
#define NIOS_PKT_RETUNE2_IDX_TIME         1
#define NIOS_PKT_RETUNE2_IDX_NIOS_PROFILE 9
#define NIOS_PKT_RETUNE2_IDX_RFFE_PROFILE 11
#define NIOS_PKT_RETUNE2_IDX_RFFE_PORT    12
#define NIOS_PKT_RETUNE2_IDX_SPDT         13
#define NIOS_PKT_RETUNE2_IDX_REQUEST_TOKEN 14

#define NIOS_PKT_RETUNE2_MAGIC            'U'

/* Specify this value instead of a timestamp to clear the retune2 queue */
#define NIOS_PKT_RETUNE2_CLEAR_QUEUE      ((uint64_t) -1)

/* Denotes that the retune2 should not be scheduled - it should occur "now" */
#define NIOS_PKT_RETUNE2_NOW              ((uint64_t) 0x00)

/* The IS_RX bit embedded in the 'port' parameter of the retune2 packet */
#define NIOS_PKT_RETUNE2_PORT_IS_RX_MASK  (0x1 << 7)

/* bladeRF2 quick-tune route fields are generated from the band map. Accept
 * only those complete encodings: masking a malformed value in the RFIC
 * helpers can otherwise silently select a different route. */
static inline bool nios_pkt_retune2_route_valid(bladerf_module module,
                                                uint8_t port,
                                                uint8_t spdt)
{
    if (module == BLADERF_MODULE_RX) {
        /* RX shutdown and high-band both select the A input port; their
         * distinct switch settings distinguish the generated profiles. */
        return (port == 0x83 && spdt == 0x00) ||
               (port == 0x8c && spdt == 0x0a) ||
               (port == 0x83 && spdt == 0x05);
    }
    if (module == BLADERF_MODULE_TX) {
        return (port == 0x00 && spdt == 0x00) ||
               (port == 0x40 && spdt == 0xa0) ||
               (port == 0x00 && spdt == 0x50);
    }
    return false;
}

/* Pack the retune2 request buffer with the provided parameters */
static inline void nios_pkt_retune2_pack_token(uint8_t *buf,
                                               bladerf_module module,
                                               uint64_t timestamp,
                                               uint16_t nios_profile,
                                               uint8_t rffe_profile,
                                               uint8_t port,
                                               uint8_t spdt,
                                               uint16_t request_token);

static inline void nios_pkt_retune2_pack(uint8_t *buf,
                                         bladerf_module module,
                                         uint64_t timestamp,
                                         uint16_t nios_profile,
                                         uint8_t rffe_profile,
                                         uint8_t port,
                                         uint8_t spdt)
{
    nios_pkt_retune2_pack_token(buf, module, timestamp, nios_profile,
                                rffe_profile, port, spdt, 0);
}

static inline void nios_pkt_retune2_pack_token(uint8_t *buf,
                                               bladerf_module module,
                                               uint64_t timestamp,
                                               uint16_t nios_profile,
                                               uint8_t rffe_profile,
                                               uint8_t port,
                                               uint8_t spdt,
                                               uint16_t request_token)
{
    uint8_t pkt_port;

    /* Clear the IS_RX bit of the port parameter */
    pkt_port = (port & (~NIOS_PKT_RETUNE2_PORT_IS_RX_MASK));

    /* Set the IS_RX bit (if needed) */
    pkt_port = (pkt_port | (BLADERF_CHANNEL_IS_TX(module) ? 0x0 :
                            NIOS_PKT_RETUNE2_PORT_IS_RX_MASK)) & 0xff;

    buf[NIOS_PKT_RETUNE2_IDX_MAGIC] = NIOS_PKT_RETUNE2_MAGIC;

    buf[NIOS_PKT_RETUNE2_IDX_TIME + 0] = (timestamp >>  0) & 0xff;
    buf[NIOS_PKT_RETUNE2_IDX_TIME + 1] = (timestamp >>  8) & 0xff;
    buf[NIOS_PKT_RETUNE2_IDX_TIME + 2] = (timestamp >> 16) & 0xff;
    buf[NIOS_PKT_RETUNE2_IDX_TIME + 3] = (timestamp >> 24) & 0xff;
    buf[NIOS_PKT_RETUNE2_IDX_TIME + 4] = (timestamp >> 32) & 0xff;
    buf[NIOS_PKT_RETUNE2_IDX_TIME + 5] = (timestamp >> 40) & 0xff;
    buf[NIOS_PKT_RETUNE2_IDX_TIME + 6] = (timestamp >> 48) & 0xff;
    buf[NIOS_PKT_RETUNE2_IDX_TIME + 7] = (timestamp >> 56) & 0xff;

    buf[NIOS_PKT_RETUNE2_IDX_NIOS_PROFILE + 0] = (nios_profile >> 0) & 0xff;
    buf[NIOS_PKT_RETUNE2_IDX_NIOS_PROFILE + 1] = (nios_profile >> 8) & 0xff;

    buf[NIOS_PKT_RETUNE2_IDX_RFFE_PROFILE] = rffe_profile & 0xff;

    buf[NIOS_PKT_RETUNE2_IDX_RFFE_PORT] = pkt_port;

    buf[NIOS_PKT_RETUNE2_IDX_SPDT] = spdt & 0xff;

    buf[NIOS_PKT_RETUNE2_IDX_REQUEST_TOKEN + 0] = request_token & 0xff;
    buf[NIOS_PKT_RETUNE2_IDX_REQUEST_TOKEN + 1] = request_token >> 8;
}

/* Unpack a retune request */
static inline void nios_pkt_retune2_unpack_token(
    const uint8_t *buf, bladerf_module *module, uint64_t *timestamp,
    uint16_t *nios_profile, uint8_t *rffe_profile, uint8_t *port,
    uint8_t *spdt, uint16_t *request_token);

static inline void nios_pkt_retune2_unpack(const uint8_t *buf,
                                           bladerf_module *module,
                                           uint64_t *timestamp,
                                           uint16_t *nios_profile,
                                           uint8_t *rffe_profile,
                                           uint8_t *port,
                                           uint8_t *spdt)
{
    nios_pkt_retune2_unpack_token(buf, module, timestamp, nios_profile,
                                  rffe_profile, port, spdt, NULL);
}

static inline void nios_pkt_retune2_unpack_token(
    const uint8_t *buf, bladerf_module *module, uint64_t *timestamp,
    uint16_t *nios_profile, uint8_t *rffe_profile, uint8_t *port,
    uint8_t *spdt, uint16_t *request_token)
{
    *timestamp  = ( ((uint64_t)buf[NIOS_PKT_RETUNE2_IDX_TIME + 0]) <<  0 );
    *timestamp |= ( ((uint64_t)buf[NIOS_PKT_RETUNE2_IDX_TIME + 1]) <<  8 );
    *timestamp |= ( ((uint64_t)buf[NIOS_PKT_RETUNE2_IDX_TIME + 2]) << 16 );
    *timestamp |= ( ((uint64_t)buf[NIOS_PKT_RETUNE2_IDX_TIME + 3]) << 24 );
    *timestamp |= ( ((uint64_t)buf[NIOS_PKT_RETUNE2_IDX_TIME + 4]) << 32 );
    *timestamp |= ( ((uint64_t)buf[NIOS_PKT_RETUNE2_IDX_TIME + 5]) << 40 );
    *timestamp |= ( ((uint64_t)buf[NIOS_PKT_RETUNE2_IDX_TIME + 6]) << 48 );
    *timestamp |= ( ((uint64_t)buf[NIOS_PKT_RETUNE2_IDX_TIME + 7]) << 56 );

    *nios_profile  = ( ((uint16_t)buf[NIOS_PKT_RETUNE2_IDX_NIOS_PROFILE + 0])
                       << 0 );
    *nios_profile |= ( ((uint16_t)buf[NIOS_PKT_RETUNE2_IDX_NIOS_PROFILE + 1])
                       << 8 );

    *rffe_profile  = buf[NIOS_PKT_RETUNE2_IDX_RFFE_PROFILE];

    *port = buf[NIOS_PKT_RETUNE2_IDX_RFFE_PORT];

    *spdt = buf[NIOS_PKT_RETUNE2_IDX_SPDT];

    if (request_token != NULL) {
        *request_token =
            (uint16_t)buf[NIOS_PKT_RETUNE2_IDX_REQUEST_TOKEN] |
            ((uint16_t)buf[NIOS_PKT_RETUNE2_IDX_REQUEST_TOKEN + 1] << 8);
    }

    *module = ( (buf[NIOS_PKT_RETUNE2_IDX_RFFE_PORT] &
                 NIOS_PKT_RETUNE2_PORT_IS_RX_MASK) ? BLADERF_MODULE_RX :
                BLADERF_MODULE_TX );

}

static inline uint16_t nios_pkt_retune2_unpack_request_token(
    const uint8_t *buf)
{
    return (uint16_t)buf[NIOS_PKT_RETUNE2_IDX_REQUEST_TOKEN] |
           ((uint16_t)buf[NIOS_PKT_RETUNE2_IDX_REQUEST_TOKEN + 1] << 8);
}


/*
 *                             Response
 *                      ----------------------
 *
 * +================+=========================================================+
 * |  Byte offset   |                       Description                       |
 * +================+=========================================================+
 * |        0       | Magic Value                                             |
 * +----------------+---------------------------------------------------------+
 * |        1       | 64-bit duration denoting how long the operation took to |
 * |                | complete, in units of timestamp ticks. (Note 1)         |
 * +----------------+---------------------------------------------------------+
 * |        9       | Status Flags (Note 2)                                   |
 * +----------------+---------------------------------------------------------+
 * |      10-11     | Host request token echoed when nonzero.                  |
 * +----------------+---------------------------------------------------------+
 * |      12-15     | Captured command-session generation (token-aware only). |
 * +----------------+---------------------------------------------------------+
 *
 * (Note 1) This value will be zero if timestamps are not running for the
 *          associated module.
 *
 * (Note 2) Token zero preserves the legacy all-zero response tail. A
 *          nonzero request token is echoed with the FPGA-captured command
 *          session generation so a late ACK cannot be mistaken for the ACK
 *          to a later scheduled request.
 *
 * (Note 3) Description of Status Flags:
 *
 *      flags[0]: 1 = Timestamp is valid. This is only the case for "Tune NOW"
 *                    requests. It is not possible to return this information
 *                    for scheduled retunes, as the event generally does not
 *                    occur before the response is set.
 *
 *                0 = This was a scheduled retune. Timestamp fields should be
 *                    ignored.
 *
 *      flags[1]: 1 = Operation completed successfully.
 *                0 = Operation failed.
 *
 *                For "Tune NOW" requests, a failure may occur as the result
 *                of the tuning algorithm failing to occur, and such other
 *                unexpected failurs.
 *
 *                The scheduled tune request will fail if the retune queue is
 *                full or if its timestamp is not later than the current
 *                module time or the last queued request.
 *
 *      flags[2]: 1 = Scheduled request was rejected because its timestamp
 *                    is not strictly in the future and after the last queued
 *                    request.
 *
 *      flags[3]: 1 = Queue clear failed because the time-tamer did not
 *                    acknowledge cancellation of its armed compare.
 *      flags[4]: 1 = Queue clear is pending its timestamp-domain cancel ACK.
 *
 *      flags[5]: 1 = Request rejected because an RFIC callback owns the
 *                    NIOS command processor. Request was not queued.
 *
 *      flags[7]      Reserved. Set to 0.
 *      flags[6]      Async NOW accepted; tokened request completes through
 *                    the correlated RETUNE2 completion/failure record.
 *                    For this response only, bytes 1-8 carry the success and
 *                    failure ring sequence baselines. Duration is unused.
 */

#define NIOS_PKT_RETUNE2_RESP_IDX_MAGIC   0
#define NIOS_PKT_RETUNE2_RESP_IDX_TIME    1
#define NIOS_PKT_RETUNE2_RESP_IDX_FLAGS   9
#define NIOS_PKT_RETUNE2_RESP_IDX_RESV    10
#define NIOS_PKT_RETUNE2_RESP_IDX_TOKEN   10
#define NIOS_PKT_RETUNE2_RESP_IDX_SESSION_GENERATION 12
#define NIOS_PKT_RETUNE2_RESP_IDX_SUCCESS_COUNT NIOS_PKT_RETUNE2_RESP_IDX_TIME
#define NIOS_PKT_RETUNE2_RESP_IDX_FAILURE_COUNT (NIOS_PKT_RETUNE2_RESP_IDX_TIME + 4)

#define NIOS_PKT_RETUNE2_RESP_FLAG_TSVTUNE_VALID (1 << 0)
#define NIOS_PKT_RETUNE2_RESP_FLAG_SUCCESS       (1 << 1)
#define NIOS_PKT_RETUNE2_RESP_FLAG_INVALID_TIMESTAMP (1 << 2)
#define NIOS_PKT_RETUNE2_RESP_FLAG_CANCEL_FAILED  (1 << 3)
#define NIOS_PKT_RETUNE2_RESP_FLAG_CANCEL_PENDING (1 << 4)
#define NIOS_PKT_RETUNE2_RESP_FLAG_BUSY           (1 << 5)
#define NIOS_PKT_RETUNE2_RESP_FLAG_ASYNC_PENDING  (1 << 6)

/* Detailed device-side cause for a failed RETUNE2 NOW request. It is encoded
 * in the otherwise unused duration field for an immediate failure, preserving
 * the legacy requirement that response bytes 10-15 remain zero. */
typedef enum {
    NIOS_PKT_RETUNE2_FAILURE_NONE = 0,
    NIOS_PKT_RETUNE2_FAILURE_INVALID_REQUEST,
    NIOS_PKT_RETUNE2_FAILURE_TIMER_READ,
    NIOS_PKT_RETUNE2_FAILURE_QUEUE_FULL,
    NIOS_PKT_RETUNE2_FAILURE_INVALID_TIMESTAMP,
    NIOS_PKT_RETUNE2_FAILURE_PROFILE_SLOT_BUSY,
    NIOS_PKT_RETUNE2_FAILURE_PROFILE_LOAD,
    NIOS_PKT_RETUNE2_FAILURE_PEER_FREQUENCY_UNKNOWN,
    NIOS_PKT_RETUNE2_FAILURE_FASTLOCK_SETUP_WRITE,
    NIOS_PKT_RETUNE2_FAILURE_PLL_STATUS_READ,
    NIOS_PKT_RETUNE2_FAILURE_PLL_LOCK_TIMEOUT,
    NIOS_PKT_RETUNE2_FAILURE_RF_PORT_SELECT,
    NIOS_PKT_RETUNE2_FAILURE_SPDT_SELECT,
    NIOS_PKT_RETUNE2_FAILURE_LO_READBACK,
    NIOS_PKT_RETUNE2_FAILURE_TX_QUAD_CALIBRATION,
    NIOS_PKT_RETUNE2_FAILURE_RFIC_FAULTED,
    NIOS_PKT_RETUNE2_FAILURE_SCHEDULE,
    NIOS_PKT_RETUNE2_FAILURE_TIMER_STOPPED,
    NIOS_PKT_RETUNE2_FAILURE_INTERNAL,
    NIOS_PKT_RETUNE2_FAILURE_CANCELLED,
    NIOS_PKT_RETUNE2_FAILURE_RFIC_CONTEXT_UNAVAILABLE,
} nios_pkt_retune2_failure;

#define NIOS_PKT_RETUNE2_FAILURE_DURATION_TAG UINT64_C(0xa75c000000000000)
#define NIOS_PKT_RETUNE2_FAILURE_DURATION_MASK UINT64_C(0xffffffffffff0000)

static inline void nios_pkt_retune2_resp_pack(uint8_t *buf,
                                              uint64_t duration,
                                              uint8_t flags,
                                              uint8_t failure,
                                              uint16_t request_token,
                                              uint32_t session_generation)
{
    if (failure != NIOS_PKT_RETUNE2_FAILURE_NONE &&
        (flags & NIOS_PKT_RETUNE2_RESP_FLAG_SUCCESS) == 0) {
        duration = NIOS_PKT_RETUNE2_FAILURE_DURATION_TAG | failure;
    }
    buf[NIOS_PKT_RETUNE2_RESP_IDX_MAGIC] = NIOS_PKT_RETUNE2_MAGIC;

    buf[NIOS_PKT_RETUNE2_RESP_IDX_TIME + 0] =  duration        & 0xff;
    buf[NIOS_PKT_RETUNE2_RESP_IDX_TIME + 1] = (duration >> 8)  & 0xff;
    buf[NIOS_PKT_RETUNE2_RESP_IDX_TIME + 2] = (duration >> 16) & 0xff;
    buf[NIOS_PKT_RETUNE2_RESP_IDX_TIME + 3] = (duration >> 24) & 0xff;
    buf[NIOS_PKT_RETUNE2_RESP_IDX_TIME + 4] = (duration >> 32) & 0xff;
    buf[NIOS_PKT_RETUNE2_RESP_IDX_TIME + 5] = (duration >> 40) & 0xff;
    buf[NIOS_PKT_RETUNE2_RESP_IDX_TIME + 6] = (duration >> 48) & 0xff;
    buf[NIOS_PKT_RETUNE2_RESP_IDX_TIME + 7] = (duration >> 56) & 0xff;

    buf[NIOS_PKT_RETUNE2_RESP_IDX_FLAGS] = flags;

    /* Token zero is reserved for legacy requests. Keep the old all-zero
     * response tail for old hosts and untracked NOW/queue-clear commands. */
    buf[NIOS_PKT_RETUNE2_RESP_IDX_TOKEN + 0] = request_token & 0xff;
    buf[NIOS_PKT_RETUNE2_RESP_IDX_TOKEN + 1] = request_token >> 8;
    buf[NIOS_PKT_RETUNE2_RESP_IDX_SESSION_GENERATION + 0] =
        request_token != 0 ? session_generation & 0xff : 0;
    buf[NIOS_PKT_RETUNE2_RESP_IDX_SESSION_GENERATION + 1] =
        request_token != 0 ? (session_generation >> 8) & 0xff : 0;
    buf[NIOS_PKT_RETUNE2_RESP_IDX_SESSION_GENERATION + 2] =
        request_token != 0 ? (session_generation >> 16) & 0xff : 0;
    buf[NIOS_PKT_RETUNE2_RESP_IDX_SESSION_GENERATION + 3] =
        request_token != 0 ? (session_generation >> 24) & 0xff : 0;
}

static inline void nios_pkt_retune2_resp_unpack(const uint8_t *buf,
                                                uint64_t      *duration,
                                                uint8_t       *flags)
{
    *duration  = buf[NIOS_PKT_RETUNE2_RESP_IDX_TIME + 0];
    *duration |= ((uint64_t) buf[NIOS_PKT_RETUNE2_RESP_IDX_TIME + 1]) << 8;
    *duration |= ((uint64_t) buf[NIOS_PKT_RETUNE2_RESP_IDX_TIME + 2]) << 16;
    *duration |= ((uint64_t) buf[NIOS_PKT_RETUNE2_RESP_IDX_TIME + 3]) << 24;
    *duration |= ((uint64_t) buf[NIOS_PKT_RETUNE2_RESP_IDX_TIME + 4]) << 32;
    *duration |= ((uint64_t) buf[NIOS_PKT_RETUNE2_RESP_IDX_TIME + 5]) << 40;
    *duration |= ((uint64_t) buf[NIOS_PKT_RETUNE2_RESP_IDX_TIME + 6]) << 48;
    *duration |= ((uint64_t) buf[NIOS_PKT_RETUNE2_RESP_IDX_TIME + 7]) << 56;

    *flags = buf[NIOS_PKT_RETUNE2_RESP_IDX_FLAGS];
}

static inline uint8_t nios_pkt_retune2_resp_failure(const uint8_t *buf)
{
    uint64_t duration;
    uint8_t failure;

    duration  = buf[NIOS_PKT_RETUNE2_RESP_IDX_TIME + 0];
    duration |= ((uint64_t) buf[NIOS_PKT_RETUNE2_RESP_IDX_TIME + 1]) << 8;
    duration |= ((uint64_t) buf[NIOS_PKT_RETUNE2_RESP_IDX_TIME + 2]) << 16;
    duration |= ((uint64_t) buf[NIOS_PKT_RETUNE2_RESP_IDX_TIME + 3]) << 24;
    duration |= ((uint64_t) buf[NIOS_PKT_RETUNE2_RESP_IDX_TIME + 4]) << 32;
    duration |= ((uint64_t) buf[NIOS_PKT_RETUNE2_RESP_IDX_TIME + 5]) << 40;
    duration |= ((uint64_t) buf[NIOS_PKT_RETUNE2_RESP_IDX_TIME + 6]) << 48;
    duration |= ((uint64_t) buf[NIOS_PKT_RETUNE2_RESP_IDX_TIME + 7]) << 56;
    if ((duration & NIOS_PKT_RETUNE2_FAILURE_DURATION_MASK) !=
        NIOS_PKT_RETUNE2_FAILURE_DURATION_TAG) {
        return NIOS_PKT_RETUNE2_FAILURE_NONE;
    }
    failure = (uint8_t)duration;
    return failure <= NIOS_PKT_RETUNE2_FAILURE_RFIC_CONTEXT_UNAVAILABLE
        ? failure : NIOS_PKT_RETUNE2_FAILURE_NONE;
}

static inline void nios_pkt_retune2_resp_set_async_baseline(
    uint8_t *buf, uint32_t success_count, uint32_t failure_count)
{
    unsigned int i;

    for (i = 0; i < 4; ++i) {
        buf[NIOS_PKT_RETUNE2_RESP_IDX_SUCCESS_COUNT + i] =
            (uint8_t)(success_count >> (8u * i));
        buf[NIOS_PKT_RETUNE2_RESP_IDX_FAILURE_COUNT + i] =
            (uint8_t)(failure_count >> (8u * i));
    }
}

static inline void nios_pkt_retune2_resp_get_async_baseline(
    const uint8_t *buf, uint32_t *success_count, uint32_t *failure_count)
{
    unsigned int i;

    *success_count = 0;
    *failure_count = 0;
    for (i = 0; i < 4; ++i) {
        *success_count |=
            (uint32_t)buf[NIOS_PKT_RETUNE2_RESP_IDX_SUCCESS_COUNT + i]
            << (8u * i);
        *failure_count |=
            (uint32_t)buf[NIOS_PKT_RETUNE2_RESP_IDX_FAILURE_COUNT + i]
            << (8u * i);
    }
}

#endif
