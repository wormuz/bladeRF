/*
 * Copyright (C) 2014 Nuand LLC
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 */

#ifndef STREAMING_METADATA_H_
#define STREAMING_METADATA_H_

#include <stddef.h>

/*
 *  Metadata layout
 * ~~~~~~~~~~~~~~~~~~~~~~~
 *
 * The FPGA handles data in units of "messages."  These messages are
 * 2048 or 8192 bytes for USB 2.0 (Hi-Speed) or USB 3.0 (SuperSpeed),
 * respectively.
 *
 * The first 16 bytes of the message form a header, which includes metadata
 * for the samples within the message. This header is shown below:
 *
 *       +-----------------+
 *  0x00 |  Packet length  |    2 bytes, Little-endian uint16_t
 *       +-----------------+
 *  0x02 |   Packet flags  |    1 byte
 *       +-----------------+
 *  0x03 |  Packet core ID |    1 byte
 *       +-----------------+
 *  0x04 |    Timestamp    |    8 bytes, Little-endian uint64_t
 *       +-----------------+
 *  0x0c |      Flags      |    4 bytes, Little-endian uint32_t
 *       +-----------------+
 *
 * The term "buffer" is used to describe a block of of data received from or
 * sent to the device. The size of a "buffer" (in bytes) is always a multiple
 * of the size of a "message." Said another way, a buffer will always evenly
 * divide into multiple messages.  Messages are *not* fragmented across
 * consecutive buffers.
 *
 *       +-----------------+ <-.  <-.
 *       | header          |   |    |
 *       +-----------------+   |    |
 *       |                 |   |    |
 *       | samples         |   |    |
 *       |                 |   |    |
 *       +-----------------+   |  <-+---- message
 *       | header          |   |
 *       +-----------------+   |
 *       |                 |   |
 *       | samples         |   |
 *       |                 |   |
 *       +-----------------+   |
 *       | header          |   |
 *       +-----------------+   |
 *       |                 |   |
 *       | samples         |   |
 *       |                 |   |
 *       +-----------------+   |
 *       | header          |   |
 *       +-----------------+   |
 *       |                 |   |
 *       | samples         |   |
 *       |                 |   |
 *       +-----------------+ <-+---------- buffer
 *
 *
 * When intentionally transmitting discontinuous groups of samples (such
 * as bursts), it is important that the last two samples within a message
 * be (0 + 0j). Otherwise, the DAC will not properly hold its output
 * at (0 + 0j) for the duration of the discontinuity.
 */

/* Components of the metadata header */
#define METADATA_RESV_SIZE (sizeof(uint32_t))
#define METADATA_TIMESTAMP_SIZE (sizeof(uint64_t))
#define METADATA_FLAGS_SIZE (sizeof(uint32_t))
#define METADATA_PACKET_LEN_SIZE (sizeof(uint16_t))
#define METADATA_PACKET_CORE_SIZE (sizeof(uint8_t))
#define METADATA_PACKET_FLAGS_SIZE (sizeof(uint8_t))

#define METADATA_RESV_OFFSET 0
#define METADATA_PACKET_LEN_OFFSET 0
#define METADATA_PACKET_FLAGS_OFFSET 2
#define METADATA_PACKET_CORE_OFFSET 3
#define METADATA_TIMESTAMP_OFFSET (METADATA_RESV_SIZE)
#define METADATA_FLAGS_OFFSET \
    (METADATA_TIMESTAMP_OFFSET + METADATA_TIMESTAMP_SIZE)

#define METADATA_HEADER_SIZE (METADATA_FLAGS_OFFSET + METADATA_FLAGS_SIZE)

/* RX sample-META epoch tag occupies the otherwise unused first dword.
 * Bit 31 distinguishes it from legacy/sample metadata headers; bits 7:0
 * carry the FPGA's epoch ID. PACKET_META continues to use the dword as its
 * packet header and must not interpret it as an epoch tag. */
#define METADATA_RX_EPOCH_TAG_VALID (1u << 31)
#define METADATA_RX_EPOCH_ID_MASK   0xffu

static inline bool metadata_get_rx_epoch_id(const uint8_t *header,
                                            uint8_t *epoch_id)
{
    uint32_t tag;
    memcpy(&tag, &header[METADATA_RESV_OFFSET], sizeof(tag));
    tag = LE32_TO_HOST(tag);

    if ((tag & METADATA_RX_EPOCH_TAG_VALID) == 0) {
        return false;
    }

    *epoch_id = (uint8_t)(tag & METADATA_RX_EPOCH_ID_MASK);
    return true;
}

/* A message is admissible in an epoch-filtered sample-META stream only when
 * it carries an explicit matching ID. Missing tags fail closed. */
static inline bool metadata_rx_epoch_matches(const uint8_t *header,
                                             uint8_t expected_epoch_id)
{
    uint8_t epoch_id;
    return metadata_get_rx_epoch_id(header, &epoch_id) &&
           epoch_id == expected_epoch_id;
}

static inline uint64_t metadata_get_timestamp(const uint8_t *header);

/* Validate every sample-META message in an async RX transfer against the
 * certified epoch and first-valid timestamp. Reject partial message buffers
 * and missing tags rather than guessing where valid IQ begins. */
static inline bool metadata_rx_buffer_matches_epoch(
    const uint8_t *buffer, size_t length, size_t message_size,
    uint8_t expected_epoch_id, uint64_t minimum_timestamp)
{
    size_t offset;

    if (buffer == NULL || message_size <= METADATA_HEADER_SIZE || length == 0 ||
        length % message_size != 0) {
        return false;
    }

    for (offset = 0; offset < length; offset += message_size) {
        const uint8_t *header = buffer + offset;
        if (!metadata_rx_epoch_matches(header, expected_epoch_id) ||
            metadata_get_timestamp(header) < minimum_timestamp) {
            return false;
        }
    }

    return true;
}

/* Header-level disposition used by sync RX. Keeping this decision pure lets
 * tests exercise the same stale-epoch, timestamp-prefix, and discontinuity
 * branches used by the stream parser. */
enum metadata_rx_epoch_disposition {
    METADATA_RX_EPOCH_ACCEPT,
    METADATA_RX_EPOCH_DROP_MESSAGE,
    METADATA_RX_EPOCH_SKIP_TIMESTAMP_PREFIX,
    METADATA_RX_EPOCH_RETURN_VALID_PREFIX,
    METADATA_RX_EPOCH_DISCONTINUITY,
};

static inline enum metadata_rx_epoch_disposition metadata_rx_epoch_disposition(
    bool boundary_enabled, bool epoch_filter_enabled, bool epoch_matches,
    uint64_t message_timestamp, uint64_t expected_timestamp,
    uint64_t minimum_timestamp, bool have_timestamp, bool copied_data)
{
    if (boundary_enabled && epoch_filter_enabled && !epoch_matches) {
        return copied_data ? METADATA_RX_EPOCH_RETURN_VALID_PREFIX :
                             METADATA_RX_EPOCH_DROP_MESSAGE;
    }

    if (boundary_enabled &&
        (message_timestamp < minimum_timestamp ||
         expected_timestamp < minimum_timestamp)) {
        return METADATA_RX_EPOCH_SKIP_TIMESTAMP_PREFIX;
    }

    if (have_timestamp && message_timestamp != expected_timestamp) {
        return METADATA_RX_EPOCH_DISCONTINUITY;
    }

    return METADATA_RX_EPOCH_ACCEPT;
}

static inline uint64_t metadata_get_timestamp(const uint8_t *header)
{
    uint64_t ret;
    assert(sizeof(ret) == METADATA_TIMESTAMP_SIZE);
    memcpy(&ret, &header[METADATA_TIMESTAMP_OFFSET], METADATA_TIMESTAMP_SIZE);

    ret = LE64_TO_HOST(ret);

    return ret;
}

static inline uint32_t metadata_get_flags(const uint8_t *header)
{
    uint32_t ret;
    assert(sizeof(ret) == METADATA_FLAGS_SIZE);
    memcpy(&ret, &header[METADATA_FLAGS_OFFSET], METADATA_FLAGS_SIZE);
    return LE32_TO_HOST(ret);
}

static inline uint16_t metadata_get_packet_len(const uint8_t *header)
{
    uint16_t ret;
    assert(sizeof(ret) == METADATA_PACKET_LEN_SIZE);
    memcpy(&ret, &header[METADATA_PACKET_LEN_OFFSET], METADATA_PACKET_LEN_SIZE);
    return LE16_TO_HOST(ret);
}

static inline uint8_t metadata_get_packet_core(const uint8_t *header)
{
    uint8_t ret;
    assert(sizeof(ret) == METADATA_PACKET_CORE_SIZE);
    memcpy(&ret, &header[METADATA_PACKET_CORE_OFFSET], METADATA_PACKET_CORE_SIZE);
    return ret;
}

static inline uint8_t metadata_get_packet_flags(const uint8_t *header)
{
    uint8_t ret;
    assert(sizeof(ret) == METADATA_PACKET_FLAGS_SIZE);
    memcpy(&ret, &header[METADATA_PACKET_FLAGS_OFFSET], METADATA_PACKET_FLAGS_SIZE);
    return ret;
}

static inline void metadata_set_packet(uint8_t *header,
                                uint64_t timestamp,
                                uint32_t flags,
                                uint16_t length,
                                uint8_t core,
                                uint8_t pkt_flags)
{
    timestamp = HOST_TO_LE64(timestamp);

    flags = HOST_TO_LE32(flags);

    length = HOST_TO_LE16(length);

    assert(sizeof(timestamp) == METADATA_TIMESTAMP_SIZE);
    assert(sizeof(flags) == METADATA_FLAGS_SIZE);

    memset(&header[METADATA_RESV_OFFSET], 0, METADATA_RESV_SIZE);

    memcpy(&header[METADATA_PACKET_LEN_OFFSET],   &length,    METADATA_PACKET_LEN_SIZE);
    memcpy(&header[METADATA_PACKET_CORE_OFFSET],  &core,      METADATA_PACKET_CORE_SIZE);
    memcpy(&header[METADATA_PACKET_FLAGS_OFFSET], &pkt_flags, METADATA_PACKET_FLAGS_SIZE);

    memcpy(&header[METADATA_TIMESTAMP_OFFSET], &timestamp,
           METADATA_TIMESTAMP_SIZE);

    memcpy(&header[METADATA_FLAGS_OFFSET], &flags, METADATA_FLAGS_SIZE);
}

static inline void metadata_set(uint8_t *header,
                                uint64_t timestamp,
                                uint32_t flags)
{
    metadata_set_packet(header, timestamp, flags, 0, 0, 0);
}

#endif
