/*
 * Copyright (c) 2015 Nuand LLC
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

#ifndef BLADERF_NIOS_PKT_16x64_H_
#define BLADERF_NIOS_PKT_16x64_H_

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/*
 * This file defines the Host <-> FPGA (NIOS II) packet formats for accesses
 * to devices/blocks with 16-bit addresses and 64-bit data
 *
 *
 *                              Request
 *                      ----------------------
 *
 * +================+=========================================================+
 * |  Byte offset   |                       Description                       |
 * +================+=========================================================+
 * |        0       | Magic Value                                             |
 * +----------------+---------------------------------------------------------+
 * |        1       | Target ID (Note 1)                                      |
 * +----------------+---------------------------------------------------------+
 * |        2       | Flags (Note 2)                                          |
 * +----------------+---------------------------------------------------------+
 * |        3       | Reserved. Set to 0x00.                                  |
 * +----------------+---------------------------------------------------------+
 * |       5:4      | 16-bit address, little-endian                           |
 * +----------------+---------------------------------------------------------+
 * |      13:6      | 64-bit data, little-endian                              |
 * +----------------+---------------------------------------------------------+
 * |      15:14     | RFIC-write transaction ID on FPGA >= 0.18.0; otherwise 0. |
 * +----------------+---------------------------------------------------------+
 *
 *
 *                              Response
 *                      ----------------------
 *
 * The response packet contains the same information as the request. Tagged
 * RFIC writes echo their transaction ID in bytes 15:14. Response bit 2
 * reports BUSY for a tagged RFIC write rejected while another callback owns
 * the RFIC; bit 1 reports success and is clear for BUSY.
 *
 * In the case of a read request, the data field will contain the read data, if
 * the read succeeded.
 *
 * (Note 1)
 *  The "Target ID" refers to the peripheral, device, or block to access.
 *  See the NIOS_PKT_16x64_TARGET_* values.
 *
 * (Note 2)
 *  The flags are defined as follows:
 *
 *    +================+========================+
 *    |      Bit(s)    |         Value          |
 *    +================+========================+
 *    |       7:3      | Reserved. Set to 0.    |
 *    +----------------+------------------------+
 *    |        2       |   1 = RFIC busy        |
 *    +----------------+------------------------+
 *    |                | Status. Only used in   |
 *    |                | response packet.       |
 *    |                | Ignored in request.    |
 *    |        1       |                        |
 *    |                |   1 = Success          |
 *    |                |   0 = Failure          |
 *    +----------------+------------------------+
 *    |        0       |   0 = Read operation   |
 *    |                |   1 = Write operation  |
 *    +----------------+------------------------+
 *
 */

#define NIOS_PKT_16x64_MAGIC         ((uint8_t) 'E')

/* Request packet indices */
#define NIOS_PKT_16x64_IDX_MAGIC      0
#define NIOS_PKT_16x64_IDX_TARGET_ID  1
#define NIOS_PKT_16x64_IDX_FLAGS      2
#define NIOS_PKT_16x64_IDX_RESV1      3
#define NIOS_PKT_16x64_IDX_ADDR       4
#define NIOS_PKT_16x64_IDX_DATA       6
#define NIOS_PKT_16x64_IDX_RESV2      14
#define NIOS_PKT_16x64_IDX_RFIC_TRANSACTION_ID 14

/* Target IDs */
#define NIOS_PKT_16x64_TARGET_AD9361  0x00
#define NIOS_PKT_16x64_TARGET_RFIC    0x01 /* RFIC control */
#define NIOS_PKT_16x64_TARGET_GAIN_TABLE_ROW 0x02 /* AD9361 ordered row commit */
#define NIOS_PKT_16x64_TARGET_AD9361_WRITE_BATCH 0x03 /* Up to 3 ordered byte writes */
#define NIOS_PKT_16x64_TARGET_AD9361_UPDATE_BITS 0x04 /* Atomic byte RMW */
#define NIOS_PKT_16x64_TARGET_RX_EPOCH_SNAPSHOT 0x05 /* status + first-valid TS low */

/* Host batch writers reserve the high byte as an old-firmware fallback
 * marker. The three 18-bit entries occupy bits 0..53; bits 54..55 remain
 * reserved even when all three entries are present. */
#define NIOS_PKT_16x64_BATCH_UNSUPPORTED_MARKER 0xa5u

static inline bool nios_pkt_16x64_batch_payload_valid(uint16_t count,
                                                       uint64_t data)
{
    uint64_t entry_mask;
    uint8_t marker = (uint8_t)(data >> 56);

    if (count == 0 || count > 3 ||
        (marker != 0 && marker != NIOS_PKT_16x64_BATCH_UNSUPPORTED_MARKER)) {
        return false;
    }

    entry_mask = (UINT64_C(1) << (18u * count)) - 1u;
    return (data & ~(entry_mask | (UINT64_C(0xff) << 56))) == 0;
}

/* UPDATE_BITS request data: marker[63:56]=0xa6, mask[55:48], masked value
 * [47:40], reserved[39:0]=0. Success response data is the resulting byte.
 * Older firmware echoes the request with success clear, allowing host-side
 * fallback to the original read+write sequence. */
#define NIOS_PKT_16x64_UPDATE_BITS_MARKER 0xa6u
#define NIOS_PKT_16x64_UPDATE_BITS_ERROR_MARKER 0x5au
#define NIOS_PKT_16x64_UPDATE_BITS_REQUEST(marker, mask, value) \
    (((uint64_t)(marker) << 56) | ((uint64_t)(mask) << 48) | \
     ((uint64_t)(value) << 40))
#define NIOS_PKT_16x64_UPDATE_BITS_RESERVED_MASK UINT64_C(0x000000ffffffffff)

/* NIOS fastlock profile cache access (FPGA >= 0.31.0).
 *
 * One NIOS profile is the 16 AD9361 fastlock program words that
 * fastlock_profile.profile_data stores. A packet carries 8 of them, so a
 * write is bounded to exactly three packets: STAGE half 0, STAGE half 1,
 * COMMIT. Only COMMIT changes fastlocks_{rx,tx}[slot]; the staged bytes are
 * discarded by every COMMIT and by a STAGE for a different direction/slot.
 *
 *    +========+===================================================+
 *    | addr   |                                                   |
 *    +========+===================================================+
 *    | 15:12  | Reserved, 0                                       |
 *    | 11:10  | Op: 0 = half 0 (words 0..7), 1 = half 1           |
 *    |        |     (words 8..15), 2 = commit / slot state         |
 *    | 9      | Reserved, 0                                       |
 *    | 8      | Direction: 0 = RX, 1 = TX                          |
 *    | 7:0    | NIOS profile slot (< NUM_BBP_FASTLOCK_PROFILES)    |
 *    +--------+---------------------------------------------------+
 *
 * Write half:   data = words, word (8*half + i) in bits [8i+7:8i].
 * Write commit: data = 0. Installs the staged words in the slot in state BBP
 *               (not RFFE-resident), so retune2 uploads them to its RFFE
 *               slot before recall.
 * Read half:    response data = the slot's words, same layout.
 * Read commit:  response data = state[7:0] | profile_num[15:8].
 *
 * A request the firmware understood but rejected returns success clear and
 * data = ERROR_MARKER[63:56] | error code. Firmware without this target
 * echoes the request data instead. */
#define NIOS_PKT_16x64_TARGET_FASTLOCK_PROFILE 0x06

#define NIOS_PKT_16x64_FASTLOCK_PROFILE_OP_HALF0  0u
#define NIOS_PKT_16x64_FASTLOCK_PROFILE_OP_HALF1  1u
#define NIOS_PKT_16x64_FASTLOCK_PROFILE_OP_COMMIT 2u
#define NIOS_PKT_16x64_FASTLOCK_PROFILE_LEN       16u
#define NIOS_PKT_16x64_FASTLOCK_PROFILE_ERROR_MARKER 0x5bu

#define NIOS_PKT_16x64_FASTLOCK_PROFILE_OK            0u
#define NIOS_PKT_16x64_FASTLOCK_PROFILE_ERR_INVALID   1u /* malformed */
#define NIOS_PKT_16x64_FASTLOCK_PROFILE_ERR_LOADING   2u /* slot upload job */
#define NIOS_PKT_16x64_FASTLOCK_PROFILE_ERR_QUEUED    3u /* retune2 entry */
#define NIOS_PKT_16x64_FASTLOCK_PROFILE_ERR_RFIC_OWNED 4u /* 0.31.0 only */
#define NIOS_PKT_16x64_FASTLOCK_PROFILE_ERR_NOT_STAGED 5u /* commit w/o halves */
#define NIOS_PKT_16x64_FASTLOCK_PROFILE_ERR_EMPTY     6u /* read INVALID slot */

/* RETUNE2 outcome records in one or two reads (FPGA >= 0.32.0). Read only.
 *
 *    +========+===================================================+
 *    | addr   |                                                   |
 *    +========+===================================================+
 *    | 15:4   | Reserved, 0                                       |
 *    | 3      | Ring: 0 = success, 1 = failure                    |
 *    | 2      | Direction: 0 = RX, 1 = TX                         |
 *    | 1:0    | Word: 0 identity, 1 execution, 2 counts, 3 synth  |
 *    +--------+---------------------------------------------------+
 *
 * Request data: the record's full 32-bit outcome sequence (words 0/1/3) or 0
 * (counts). NIOS answers from the ring slot only if it still holds exactly
 * that sequence, so each word is self-checking and a record overwritten
 * between the two reads fails the second one.
 *
 * Word 0, identity:  [15:0] request token, [23:16] NIOS profile,
 *                    [31:24] NIOS_PKT_RETUNE2_EXEC_* flags (success ring),
 *                    [39:32] command session generation, low 8 bits,
 *                    [47:40] failure stage (failure ring), [63:48] 0.
 * Word 1, execution (success ring only):
 *                    [31:0] executed - scheduled timestamp (valid with
 *                    EXEC_TS_VALID; NIOS clears that flag if the difference
 *                    does not fit), [63:32] NIOS LO readback in units of
 *                    2 Hz, 0 when not read.
 * Word 2, counts:    [31:0] success count, [63:32] failure count.
 * Word 3, synth (success ring only, FPGA >= 0.33.0):
 *                    [7:0] NIOS_PKT_RETUNE2_EXEC_* flags as recorded,
 *                    [63:8] the NIOS_PKT_RETUNE2_SYNTH_LEN bytes read over
 *                    SPI after the lock bit, byte i at [8i+15:8i+8].
 *
 * Errors: success clear, data = ERROR_MARKER[63:56] | code. */
#define NIOS_PKT_16x64_TARGET_RETUNE2_RECORD 0x07
/* Whole RX gain table from the NIOS copy (RX-TRANSITION-1S3-001); encoding
 * and contract at nios_pkt_16x64_gain_table_region_addr() below. */
#define NIOS_PKT_16x64_TARGET_GAIN_TABLE_REGION 0x08

#define NIOS_PKT_16x64_RETUNE2_RECORD_WORD_IDENTITY  0u
#define NIOS_PKT_16x64_RETUNE2_RECORD_WORD_EXECUTION 1u
#define NIOS_PKT_16x64_RETUNE2_RECORD_WORD_COUNTS    2u
#define NIOS_PKT_16x64_RETUNE2_RECORD_WORD_SYNTH     3u
#define NIOS_PKT_16x64_RETUNE2_RECORD_WORD_TIMING    4u
#define NIOS_PKT_16x64_RETUNE2_RECORD_WORD_GAIN_DETAIL 5u
/* TIMING word: four 15-bit stage durations, in units of 64 time-tamer ticks,
 * each followed by its own valid bit. */
#define NIOS_PKT_16x64_RETUNE2_TIMING_TICKS_PER_UNIT 64u
#define NIOS_PKT_16x64_RETUNE2_TIMING_VALUE_MASK     0x7fffu
#define NIOS_PKT_16x64_RETUNE2_TIMING_PROFILE_SHIFT  0u
#define NIOS_PKT_16x64_RETUNE2_TIMING_PROFILE_VALID  (UINT64_C(1) << 15)
#define NIOS_PKT_16x64_RETUNE2_TIMING_GAIN_SHIFT     16u
#define NIOS_PKT_16x64_RETUNE2_TIMING_GAIN_VALID     (UINT64_C(1) << 31)
#define NIOS_PKT_16x64_RETUNE2_TIMING_PLL_SHIFT      32u
#define NIOS_PKT_16x64_RETUNE2_TIMING_PLL_VALID      (UINT64_C(1) << 47)
#define NIOS_PKT_16x64_RETUNE2_TIMING_OTHER_SHIFT    48u
#define NIOS_PKT_16x64_RETUNE2_TIMING_OTHER_VALID    (UINT64_C(1) << 63)
/* Gap is RX time-tamer ticks; SPI and arbiter time are sys_clock microseconds. */
#define NIOS_PKT_16x64_RETUNE2_GAIN_DETAIL_VALUE_MASK 0x000fffffu
#define NIOS_PKT_16x64_RETUNE2_GAIN_DETAIL_GAP_SHIFT 0u
#define NIOS_PKT_16x64_RETUNE2_GAIN_DETAIL_SPI_SHIFT 20u
#define NIOS_PKT_16x64_RETUNE2_GAIN_DETAIL_ARBITER_SHIFT 40u
#define NIOS_PKT_16x64_RETUNE2_GAIN_DETAIL_GAP_VALID (UINT64_C(1) << 60)
#define NIOS_PKT_16x64_RETUNE2_GAIN_DETAIL_SPI_VALID (UINT64_C(1) << 61)
#define NIOS_PKT_16x64_RETUNE2_GAIN_DETAIL_ARBITER_VALID (UINT64_C(1) << 62)
#define NIOS_PKT_16x64_RETUNE2_RECORD_ERROR_MARKER   0x5cu
#define NIOS_PKT_16x64_RETUNE2_RECORD_ERR_INVALID    1u
#define NIOS_PKT_16x64_RETUNE2_RECORD_ERR_SEQUENCE   2u /* not / no longer */

static inline uint16_t nios_pkt_16x64_retune2_record_addr(bool failure,
                                                          bool tx,
                                                          uint8_t word)
{
    return (uint16_t)((failure ? 0x8u : 0u) | (tx ? 0x4u : 0u) |
                      (word & 0x3u) | ((word & 0x4u) << 2));
}

static inline bool nios_pkt_16x64_retune2_record_addr_unpack(
    uint16_t addr, bool *failure, bool *tx, uint8_t *word)
{
    const uint8_t decoded_word = (uint8_t)((addr & 0x3u) |
                                           ((addr & 0x10u) >> 2));

    if ((addr & 0xffe0u) != 0 ||
        decoded_word > NIOS_PKT_16x64_RETUNE2_RECORD_WORD_GAIN_DETAIL) {
        return false;
    }
    *failure = (addr & 0x8u) != 0;
    *tx = (addr & 0x4u) != 0;
    *word = decoded_word;
    return true;
}

static inline uint64_t nios_pkt_16x64_retune2_record_identity(
    uint16_t token, uint8_t nios_profile, uint8_t flags,
    uint8_t session_low, uint8_t failure_stage)
{
    return (uint64_t)token | ((uint64_t)nios_profile << 16) |
           ((uint64_t)flags << 24) | ((uint64_t)session_low << 32) |
           ((uint64_t)failure_stage << 40);
}

static inline uint64_t nios_pkt_16x64_retune2_record_error(uint8_t code)
{
    return ((uint64_t)NIOS_PKT_16x64_RETUNE2_RECORD_ERROR_MARKER << 56) |
           code;
}

static inline uint16_t nios_pkt_16x64_fastlock_profile_addr(bool tx,
                                                            uint8_t slot,
                                                            uint8_t op)
{
    return (uint16_t)(((uint16_t)(op & 0x3u) << 10) |
                      (tx ? (uint16_t)0x100u : (uint16_t)0u) | slot);
}

/* Returns false for reserved bits or an undefined op. */
static inline bool nios_pkt_16x64_fastlock_profile_addr_unpack(
    uint16_t addr, bool *tx, uint8_t *slot, uint8_t *op)
{
    const uint8_t decoded_op = (uint8_t)((addr >> 10) & 0x3u);

    if ((addr & 0xf200u) != 0 ||
        decoded_op > NIOS_PKT_16x64_FASTLOCK_PROFILE_OP_COMMIT) {
        return false;
    }
    if (tx != NULL) {
        *tx = (addr & 0x100u) != 0;
    }
    if (slot != NULL) {
        *slot = (uint8_t)(addr & 0xffu);
    }
    if (op != NULL) {
        *op = decoded_op;
    }
    return true;
}

static inline uint64_t nios_pkt_16x64_fastlock_profile_half_pack(
    const uint8_t *words)
{
    uint64_t data = 0;
    unsigned int i;

    for (i = 0; i < 8u; i++) {
        data |= (uint64_t)words[i] << (8u * i);
    }
    return data;
}

static inline void nios_pkt_16x64_fastlock_profile_half_unpack(
    uint64_t data, uint8_t *words)
{
    unsigned int i;

    for (i = 0; i < 8u; i++) {
        words[i] = (uint8_t)(data >> (8u * i));
    }
}

static inline uint64_t nios_pkt_16x64_fastlock_profile_error(uint8_t code)
{
    return ((uint64_t)NIOS_PKT_16x64_FASTLOCK_PROFILE_ERROR_MARKER << 56) |
           code;
}

/* Returns the error code of an understood rejection, or -1 when the response
 * data is not an error record (old firmware echo). */
static inline int nios_pkt_16x64_fastlock_profile_error_code(uint64_t data)
{
    if ((data >> 56) != NIOS_PKT_16x64_FASTLOCK_PROFILE_ERROR_MARKER ||
        (data & UINT64_C(0x00ffffffffffff00)) != 0) {
        return -1;
    }
    return (int)(data & 0xffu);
}

/* IDs 0x80 through 0xff will not be assigned by Nuand. These are reserved
 * for user customizations */
#define NIOS_PKT_16x64_TARGET_USR1    0x80
#define NIOS_PKT_16x64_TARGET_USR128  0xff

/* Flag bits */
#define NIOS_PKT_16x64_FLAG_WRITE     (1 << 0)
#define NIOS_PKT_16x64_FLAG_SUCCESS   (1 << 1)
#define NIOS_PKT_16x64_FLAG_BUSY      (1 << 2)
#define NIOS_PKT_16x64_FLAG_QUEUE_FULL (1 << 3) /* FPGA >= v0.35.0 */

/**
 * Sub-addresses for rfic target.
 *
 *    +================+============================================+
 *    |      Bit(s)    |         Value                              |
 *    +================+============================================+
 *    |      15:12     | Reserved. Set to 0.                        |
 *    +----------------+--------------------------------------------+
 *    |      11:8      | bladerf_channel & 0xf                      |
 *    |                | 1111 = system-wide                         |
 *    +----------------+--------------------------------------------+
 *    |       7:0      | Subaddress. See bladerf_rfic_command enum. |
 *    +----------------+--------------------------------------------+
 */

/* Pack the request buffer */
static inline void nios_pkt_16x64_pack(uint8_t *buf, uint8_t target, bool write,
                                       uint16_t addr, uint64_t data)
{
    buf[NIOS_PKT_16x64_IDX_MAGIC]     = NIOS_PKT_16x64_MAGIC;
    buf[NIOS_PKT_16x64_IDX_TARGET_ID] = target;

    if (write) {
        buf[NIOS_PKT_16x64_IDX_FLAGS] = NIOS_PKT_16x64_FLAG_WRITE;
    } else {
        buf[NIOS_PKT_16x64_IDX_FLAGS] = 0x00;
    }

    buf[NIOS_PKT_16x64_IDX_RESV1] = 0x00;

    buf[NIOS_PKT_16x64_IDX_ADDR + 0] = (addr >> 0);
    buf[NIOS_PKT_16x64_IDX_ADDR + 1] = (addr >> 8);

    buf[NIOS_PKT_16x64_IDX_DATA + 0] = (data >> 0)  & 0xff;
    buf[NIOS_PKT_16x64_IDX_DATA + 1] = (data >> 8)  & 0xff;
    buf[NIOS_PKT_16x64_IDX_DATA + 2] = (data >> 16) & 0xff;
    buf[NIOS_PKT_16x64_IDX_DATA + 3] = (data >> 24) & 0xff;
    buf[NIOS_PKT_16x64_IDX_DATA + 4] = (data >> 32) & 0xff;
    buf[NIOS_PKT_16x64_IDX_DATA + 5] = (data >> 40) & 0xff;
    buf[NIOS_PKT_16x64_IDX_DATA + 6] = (data >> 48) & 0xff;
    buf[NIOS_PKT_16x64_IDX_DATA + 7] = (data >> 56) & 0xff;

    buf[NIOS_PKT_16x64_IDX_RESV2 + 0] = 0x00;
    buf[NIOS_PKT_16x64_IDX_RESV2 + 1] = 0x00;
}

/* RFIC writes on FPGA >= 0.18.0 may carry a host transaction token in the
 * otherwise-reserved trailing 16 bits. All other packet forms keep zeroes. */
static inline void nios_pkt_16x64_pack_with_transaction_id(
    uint8_t *buf, uint8_t target, bool write, uint16_t addr, uint64_t data,
    uint16_t transaction_id)
{
    nios_pkt_16x64_pack(buf, target, write, addr, data);
    buf[NIOS_PKT_16x64_IDX_RFIC_TRANSACTION_ID] = transaction_id & 0xff;
    buf[NIOS_PKT_16x64_IDX_RFIC_TRANSACTION_ID + 1] = transaction_id >> 8;
}

static inline uint16_t nios_pkt_16x64_transaction_id(const uint8_t *buf)
{
    return (uint16_t)buf[NIOS_PKT_16x64_IDX_RFIC_TRANSACTION_ID] |
           ((uint16_t)buf[NIOS_PKT_16x64_IDX_RFIC_TRANSACTION_ID + 1] << 8);
}

/* Unpack the request buffer */
static inline void nios_pkt_16x64_unpack(const uint8_t *buf, uint8_t *target,
                                         bool *write, uint16_t *addr,
                                         uint64_t *data)
{
    if (target != NULL) {
        *target = buf[NIOS_PKT_16x64_IDX_TARGET_ID];
    }

    if (write != NULL) {
        *write  = (buf[NIOS_PKT_16x64_IDX_FLAGS] & NIOS_PKT_16x64_FLAG_WRITE) != 0;
    }

    if (addr != NULL) {
        *addr   = (buf[NIOS_PKT_16x64_IDX_ADDR + 0] << 0)  |
                  (buf[NIOS_PKT_16x64_IDX_ADDR + 1] << 8);
    }

    if (data != NULL) {
        *data   = ((uint64_t) buf[NIOS_PKT_16x64_IDX_DATA + 0] << 0)  |
                  ((uint64_t) buf[NIOS_PKT_16x64_IDX_DATA + 1] << 8)  |
                  ((uint64_t) buf[NIOS_PKT_16x64_IDX_DATA + 2] << 16) |
                  ((uint64_t) buf[NIOS_PKT_16x64_IDX_DATA + 3] << 24) |
                  ((uint64_t) buf[NIOS_PKT_16x64_IDX_DATA + 4] << 32) |
                  ((uint64_t) buf[NIOS_PKT_16x64_IDX_DATA + 5] << 40) |
                  ((uint64_t) buf[NIOS_PKT_16x64_IDX_DATA + 6] << 48) |
                  ((uint64_t) buf[NIOS_PKT_16x64_IDX_DATA + 7] << 56);
    }
}

/* Pack the response buffer */
static inline void nios_pkt_16x64_resp_pack(uint8_t *buf, uint8_t target,
                                           bool write, uint16_t addr,
                                           uint64_t data, bool success)
{
    nios_pkt_16x64_pack(buf, target, write, addr, data);

    if (success) {
        buf[NIOS_PKT_16x64_IDX_FLAGS] |= NIOS_PKT_16x64_FLAG_SUCCESS;
    }
}

static inline void nios_pkt_16x64_resp_pack_with_transaction_id(
    uint8_t *buf, uint8_t target, bool write, uint16_t addr, uint64_t data,
    bool success, uint16_t transaction_id)
{
    nios_pkt_16x64_resp_pack(buf, target, write, addr, data, success);
    buf[NIOS_PKT_16x64_IDX_RFIC_TRANSACTION_ID] = transaction_id & 0xff;
    buf[NIOS_PKT_16x64_IDX_RFIC_TRANSACTION_ID + 1] = transaction_id >> 8;
}

/* A tagged RFIC request observed while another ADI callback owns the RFIC
 * can be rejected without entering the ADI driver. BUSY is response-only;
 * callers may retry the same transaction after the active operation retires. */
static inline void nios_pkt_16x64_resp_pack_busy(
    uint8_t *buf, uint8_t target, uint16_t addr, uint64_t data,
    uint16_t transaction_id)
{
    nios_pkt_16x64_resp_pack_with_transaction_id(
        buf, target, true, addr, data, false, transaction_id);
    buf[NIOS_PKT_16x64_IDX_FLAGS] |= NIOS_PKT_16x64_FLAG_BUSY;
}

static inline bool nios_pkt_16x64_resp_is_busy(const uint8_t *buf)
{
    return (buf[NIOS_PKT_16x64_IDX_FLAGS] & NIOS_PKT_16x64_FLAG_BUSY) != 0;
}

/* An RFIC write refused because the command queue is at its depth: the
 * physical ring for untagged writes, or BLADERF_RFIC_TAGGED_QUEUE_DEPTH for
 * the tagged stop-and-wait protocol. The command was not accepted and will
 * never execute. Response-only. */
static inline void nios_pkt_16x64_resp_pack_queue_full(
    uint8_t *buf, uint8_t target, uint16_t addr, uint64_t data,
    uint16_t transaction_id)
{
    nios_pkt_16x64_resp_pack_with_transaction_id(
        buf, target, true, addr, data, false, transaction_id);
    buf[NIOS_PKT_16x64_IDX_FLAGS] |= NIOS_PKT_16x64_FLAG_QUEUE_FULL;
}

static inline bool nios_pkt_16x64_resp_is_queue_full(const uint8_t *buf)
{
    return (buf[NIOS_PKT_16x64_IDX_FLAGS] &
            NIOS_PKT_16x64_FLAG_QUEUE_FULL) != 0;
}

/* Unpack the response buffer */
static inline void nios_pkt_16x64_resp_unpack(const uint8_t *buf,
                                             uint8_t *target, bool *write,
                                             uint16_t *addr, uint64_t *data,
                                             bool *success)
{
    nios_pkt_16x64_unpack(buf, target, write, addr, data);

    if ((buf[NIOS_PKT_16x64_IDX_FLAGS] & NIOS_PKT_16x64_FLAG_SUCCESS) != 0) {
        *success = true;
    } else {
        *success = false;
    }
}

/* NIOS_PKT_16x64_TARGET_GAIN_TABLE_REGION (RX-TRANSITION-1S3-001)
 *
 * The host ADI driver reloads the whole 77-row RX gain table on every RX
 * gain-table band change. One NIOS_PKT_16x64_TARGET_GAIN_TABLE_ROW request
 * per row cost ~77 USB round trips (~21 ms); this target writes every row
 * from the NIOS copy of the same ADI table in one request.
 *
 * addr: bits 1:0 region (nios_pkt_retune2_gain_table_region of the table's
 *       end frequency), bit 2 split table, bits 15:8 row count
 *       (gain_table_info.max_index); other bits zero.
 * read:  data = nios_pkt_16x64_gain_table_digest() of the NIOS rows. The host
 *        uses the region write only when this equals the digest of its own
 *        table; a failed read (an image without this target) keeps the
 *        per-row path.
 * write: data bits 7:0 LNA bits ORed into each row's first byte, bits 15:8
 *        REG_GAIN_TABLE_CONFIG value written to commit each row (the same
 *        value the row request carries); other bits zero. NIOS writes rows
 *        0..count-1 with the row request's sequence and settle time. */
static inline uint16_t nios_pkt_16x64_gain_table_region_addr(
    uint8_t region, bool split, uint8_t rows)
{
    return (uint16_t)((region & 0x3u) | (split ? 0x4u : 0u) |
                      ((uint16_t)rows << 8));
}

static inline bool nios_pkt_16x64_gain_table_region_unpack(
    uint16_t addr, uint8_t *region, bool *split, uint8_t *rows)
{
    if ((addr & 0x00f8u) != 0 || (addr & 0x3u) > 2) {
        return false;
    }
    *region = (uint8_t)(addr & 0x3u);
    *split = (addr & 0x4u) != 0;
    *rows = (uint8_t)(addr >> 8);
    return *rows != 0;
}

/* FNV-1a over the three bytes of each row. */
static inline uint64_t nios_pkt_16x64_gain_table_digest(
    uint8_t (*tab)[3], uint8_t rows)
{
    uint64_t digest = UINT64_C(1469598103934665603);
    uint8_t r, k;

    for (r = 0; r < rows; ++r) {
        for (k = 0; k < 3; ++k) {
            digest = (digest ^ tab[r][k]) * UINT64_C(1099511628211);
        }
    }
    return digest;
}

#endif
