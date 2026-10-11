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

#include <limits.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <libbladeRF.h>

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

/* Async TX PACKET_META callbacks report transfer length in 32-bit DWORDs.
 * Validate it against the allocated buffer before a USB backend multiplies
 * by sizeof(uint32_t), so malformed callbacks cannot submit an out-of-bounds
 * read from the caller's buffer. */
static inline bool metadata_tx_packet_length_bytes(
    size_t buffer_bytes, unsigned int actual_count, size_t *length_bytes)
{
    const size_t header_dwords = METADATA_HEADER_SIZE / sizeof(uint32_t);
    const size_t max_dwords = buffer_bytes / sizeof(uint32_t);

    if (length_bytes == NULL ||
        (METADATA_HEADER_SIZE % sizeof(uint32_t)) != 0 ||
        actual_count < header_dwords || actual_count > max_dwords) {
        return false;
    }

    *length_bytes = (size_t)actual_count * sizeof(uint32_t);
    return true;
}

/* Synchronous PACKET_META takes a payload length in DWORDs and creates the
 * four-DWORD header itself. Bound the complete packet against its allocated
 * transfer buffer and the payload's uint16_t header field before writing. */
static inline bool metadata_tx_packet_payload_length_bytes(
    size_t buffer_bytes, unsigned int payload_dwords, size_t *length_bytes)
{
    const size_t header_dwords = METADATA_HEADER_SIZE / sizeof(uint32_t);

    if (header_dwords > UINT_MAX || payload_dwords > UINT16_MAX ||
        payload_dwords > UINT_MAX - header_dwords) {
        return false;
    }

    return metadata_tx_packet_length_bytes(
        buffer_bytes, payload_dwords + (unsigned int)header_dwords,
        length_bytes);
}

/* RX sample-META epoch tag occupies the otherwise unused first dword.
 * Bit 31 distinguishes it from legacy/sample metadata headers; bits 7:0
 * carry the FPGA's epoch ID. PACKET_META continues to use the dword as its
 * packet header and must not interpret it as an epoch tag. */
#define METADATA_RX_EPOCH_TAG_VALID (1u << 31)
#define METADATA_RX_EPOCH_ID_MASK   0xffu
#define METADATA_RX_CLIPPING_RX1    (1u << 8)
#define METADATA_RX_CLIPPING_RX2    (1u << 9)

static inline uint8_t metadata_get_rx_clipping_flags(const uint8_t *header)
{
    uint32_t tag;
    memcpy(&tag, &header[METADATA_RESV_OFFSET], sizeof(tag));
    tag = LE32_TO_HOST(tag);
    if ((tag & METADATA_RX_EPOCH_TAG_VALID) == 0) {
        return 0;
    }
    return (uint8_t)((tag >> 8) & 0x3u);
}

static inline bool metadata_rx_format_has_epoch_tag(bladerf_format format)
{
    return format == BLADERF_FORMAT_SC16_Q11_META ||
           format == BLADERF_FORMAT_SC8_Q7_META;
}

static inline bool metadata_rx_format_allowed_for_epoch_contract(
    bool epoch_contract_enabled, bladerf_format format)
{
    return !epoch_contract_enabled ||
           metadata_rx_format_has_epoch_tag(format);
}

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

/* FPGA timestamps are modulo-2^64 counters. Compare their forward distance
 * inside the unambiguous half-range so a valid timestamp after wrap remains
 * newer than its first-valid boundary. Epoch spans are required to stay
 * below 2^63 ticks; the exactly-half-range case is therefore rejected. */
static inline bool metadata_timestamp_at_or_after(uint64_t timestamp,
                                                  uint64_t boundary)
{
    return (timestamp - boundary) < (UINT64_C(1) << 63);
}

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
            !metadata_timestamp_at_or_after(
                metadata_get_timestamp(header), minimum_timestamp)) {
            return false;
        }
    }

    return true;
}

/* Identify the first rejected sample-META message when its epoch tag is
 * present. This lets the event path preserve delayed packet provenance even
 * after the host has already certified a newer epoch. Malformed or untagged
 * buffers deliberately have no attributable source. */
static inline bool metadata_rx_buffer_invalid_source(
    const uint8_t *buffer, size_t length, size_t message_size,
    uint8_t expected_epoch_id, uint64_t minimum_timestamp,
    uint8_t *source_epoch_id, uint64_t *source_timestamp)
{
    if (buffer == NULL || source_epoch_id == NULL ||
        source_timestamp == NULL || message_size <= METADATA_HEADER_SIZE ||
        length == 0 || length % message_size != 0) {
        return false;
    }

    for (size_t offset = 0; offset < length; offset += message_size) {
        const uint8_t *header = buffer + offset;
        uint8_t observed_epoch_id;
        const uint64_t timestamp = metadata_get_timestamp(header);
        if (!metadata_get_rx_epoch_id(header, &observed_epoch_id)) {
            return false;
        }
        if (observed_epoch_id != expected_epoch_id ||
            !metadata_timestamp_at_or_after(timestamp, minimum_timestamp)) {
            *source_epoch_id = observed_epoch_id;
            *source_timestamp = timestamp;
            return true;
        }
    }

    return false;
}

enum metadata_rx_buffer_epoch_result {
    METADATA_RX_BUFFER_INVALID,
    METADATA_RX_BUFFER_CONTIGUOUS,
    METADATA_RX_BUFFER_DISCONTINUITY,
};

/* The async RX parser validates a whole transfer outside its state mutex.
 * Recheck the certificate snapshot under that mutex immediately before
 * advancing the timestamp cursor/admitting the transfer. */
static inline bool metadata_rx_epoch_snapshot_is_current(
    bool snapshot_contract_enabled, bool snapshot_certified,
    uint8_t snapshot_epoch_id, uint64_t snapshot_first_valid_timestamp,
    bool current_contract_enabled, bool current_certified,
    uint8_t current_epoch_id, uint64_t current_first_valid_timestamp)
{
    return snapshot_contract_enabled && snapshot_certified &&
           current_contract_enabled && current_certified &&
           snapshot_epoch_id == current_epoch_id &&
           snapshot_first_valid_timestamp == current_first_valid_timestamp;
}

/* A delayed callback that observed an uncertified interval may clear the
 * async timestamp cursor only if that same transition is still uncertified.
 * It must not erase a cursor committed by a newer valid epoch. */
static inline bool metadata_rx_uncertified_snapshot_is_current(
    uint32_t snapshot_transaction_id, uint8_t snapshot_epoch_id,
    uint32_t current_transaction_id, uint8_t current_epoch_id,
    bool current_certified)
{
    return !current_certified &&
           snapshot_transaction_id == current_transaction_id &&
           snapshot_epoch_id == current_epoch_id;
}

/* Commit a successfully parsed transfer only if its certificate snapshot is
 * still current. Callers hold the epoch-state lock across this decision and
 * cursor update so invalidation cannot interleave with the commit. */
static inline bool metadata_rx_epoch_commit_timestamp(
    bool snapshot_contract_enabled, bool snapshot_certified,
    uint8_t snapshot_epoch_id, uint64_t snapshot_first_valid_timestamp,
    bool current_contract_enabled, bool current_certified,
    uint8_t current_epoch_id, uint64_t current_first_valid_timestamp,
    uint64_t next_timestamp, bool *have_expected_timestamp,
    uint8_t *timestamp_epoch_id, uint64_t *expected_timestamp)
{
    if (have_expected_timestamp == NULL || timestamp_epoch_id == NULL ||
        expected_timestamp == NULL ||
        !metadata_rx_epoch_snapshot_is_current(
            snapshot_contract_enabled, snapshot_certified,
            snapshot_epoch_id, snapshot_first_valid_timestamp,
            current_contract_enabled, current_certified,
            current_epoch_id, current_first_valid_timestamp)) {
        return false;
    }

    *have_expected_timestamp = true;
    *timestamp_epoch_id = snapshot_epoch_id;
    *expected_timestamp = next_timestamp;
    return true;
}

/* Validate epoch identity, the first-valid lower bound, and timestamp
 * continuity for every message in a USB transfer. timestamp_step is in FPGA
 * sample-clock ticks (divide interleaved RX_X2 samples by two). On a
 * discontinuity, next_timestamp still follows the last observed message so a
 * caller can withhold this transfer and resume at the next contiguous one. */
static inline enum metadata_rx_buffer_epoch_result
metadata_rx_buffer_epoch_contiguous(
    const uint8_t *buffer, size_t length, size_t message_size,
    uint8_t expected_epoch_id, uint64_t minimum_timestamp,
    bool have_expected_timestamp, uint64_t expected_timestamp,
    uint64_t timestamp_step, uint64_t *next_timestamp)
{
    size_t offset;
    bool discontinuity = false;

    if (timestamp_step == 0 || next_timestamp == NULL ||
        !metadata_rx_buffer_matches_epoch(buffer, length, message_size,
                                          expected_epoch_id,
                                          minimum_timestamp)) {
        return METADATA_RX_BUFFER_INVALID;
    }

    for (offset = 0; offset < length; offset += message_size) {
        const uint64_t timestamp = metadata_get_timestamp(buffer + offset);
        if (have_expected_timestamp && timestamp != expected_timestamp) {
            discontinuity = true;
        }
        expected_timestamp = timestamp + timestamp_step;
        have_expected_timestamp = true;
    }

    *next_timestamp = expected_timestamp;
    return discontinuity ? METADATA_RX_BUFFER_DISCONTINUITY :
                           METADATA_RX_BUFFER_CONTIGUOUS;
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

/* sync_rx() can return a contiguous, epoch-tagged prefix while setting the
 * overrun flag for a later gap in the same request. That prefix is still
 * host-visible IQ; consumers must receive both the data-valid event and the
 * separate discontinuity notification. */
static inline bool metadata_rx_has_epoch_samples(
    const struct bladerf_metadata *metadata)
{
    return metadata != NULL && metadata->actual_count != 0 &&
           metadata->rx_epoch_id_valid != 0;
}

static inline enum metadata_rx_epoch_disposition metadata_rx_epoch_disposition(
    bool boundary_enabled, bool epoch_filter_enabled, bool epoch_matches,
    uint64_t message_timestamp, uint64_t expected_timestamp,
    uint64_t minimum_timestamp, bool have_timestamp, bool copied_data)
{
    if (boundary_enabled && epoch_filter_enabled && !epoch_matches) {
        return copied_data ? METADATA_RX_EPOCH_RETURN_VALID_PREFIX :
                             METADATA_RX_EPOCH_DROP_MESSAGE;
    }

    /* `expected_timestamp` is undefined until a first META header seeds the
     * continuity cursor. Comparing its zero-initialized value to the epoch
     * boundary falsely classified a packet that starts exactly at the
     * boundary as a withheld timestamp prefix. */
    if (boundary_enabled &&
        (!metadata_timestamp_at_or_after(message_timestamp,
                                         minimum_timestamp) ||
         (have_timestamp &&
          !metadata_timestamp_at_or_after(expected_timestamp,
                                          minimum_timestamp)))) {
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

/* Convert FPGA RX metadata status flags to the public synchronous API status
 * namespace. The FPGA's RX_HW_UNDERFLOW bit is reported in RX metadata when
 * the TX sample FIFO starves; its bit position overlaps META_STATUS_OVERRUN,
 * so it must be translated explicitly rather than copied verbatim. */
static inline uint32_t metadata_flags_to_sync_status(uint32_t flags)
{
    uint32_t status = flags &
        (BLADERF_META_FLAG_RX_HW_MINIEXP1 |
         BLADERF_META_FLAG_RX_HW_MINIEXP2);

    if (flags & BLADERF_META_FLAG_RX_HW_UNDERFLOW) {
        status |= BLADERF_META_STATUS_UNDERRUN;
    }

    return status;
}

static inline uint16_t metadata_get_packet_len(const uint8_t *header)
{
    uint16_t ret;
    assert(sizeof(ret) == METADATA_PACKET_LEN_SIZE);
    memcpy(&ret, &header[METADATA_PACKET_LEN_OFFSET], METADATA_PACKET_LEN_SIZE);
    return LE16_TO_HOST(ret);
}

/* Validate the payload length carried by an RX PACKET_META header against
 * both the bytes received from USB and the allocated transfer buffer. The
 * length field counts payload DWORDs and does not include this header. */
static inline bool metadata_rx_packet_payload_length_valid(
    const uint8_t *buffer, size_t received_bytes, size_t buffer_bytes)
{
    size_t payload_bytes;
    uint16_t payload_dwords;

    if (buffer == NULL || received_bytes < METADATA_HEADER_SIZE ||
        received_bytes > buffer_bytes) {
        return false;
    }

    payload_dwords = metadata_get_packet_len(buffer);
    payload_bytes = (size_t)payload_dwords * sizeof(uint32_t);
    return payload_bytes <= received_bytes - METADATA_HEADER_SIZE;
}

static inline bool metadata_rx_packet_fits_output(const uint8_t *header,
                                                  size_t output_dwords)
{
    return header != NULL && metadata_get_packet_len(header) <= output_dwords;
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
