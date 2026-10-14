/* Copyright (c) 2026 Nuand LLC */

#ifndef FPGA_COMMON_RFIC_PROGRESS_POLICY_H_
#define FPGA_COMMON_RFIC_PROGRESS_POLICY_H_

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

/* Stable protocol results. Never transmit the toolchain's errno number: its
 * numeric values are not part of the host/firmware wire ABI. */
enum bladerf_rfic_result_code {
    BLADERF_RFIC_RESULT_SUCCESS = 0,
    BLADERF_RFIC_RESULT_TIMEOUT = 1,
    BLADERF_RFIC_RESULT_INVALID_PARAMETER = 2,
    BLADERF_RFIC_RESULT_IO = 3,
    BLADERF_RFIC_RESULT_UNSUPPORTED = 4,
    BLADERF_RFIC_RESULT_INTERNAL = 5,
    BLADERF_RFIC_RESULT_NO_RESULT = 0xff,
};

/* Version 0.24 terminal detail record. errno values stay private to NIOS;
 * these stable result classes let the host observe both the primary error
 * and an independent cleanup failure. */
struct bladerf_rfic_result_detail {
    uint32_t sequence;
    uint16_t transaction_id;
    uint8_t primary_result;
    uint8_t cleanup_result;
};

static inline uint64_t bladerf_rfic_result_detail_pack(
    const struct bladerf_rfic_result_detail *detail)
{
    return ((uint64_t)detail->sequence << 32) |
           ((uint64_t)detail->cleanup_result << 24) |
           ((uint64_t)detail->primary_result << 16) |
           detail->transaction_id;
}

static inline struct bladerf_rfic_result_detail
bladerf_rfic_result_detail_unpack(uint64_t value)
{
    struct bladerf_rfic_result_detail detail;

    detail.sequence = (uint32_t)(value >> 32);
    detail.transaction_id = (uint16_t)value;
    detail.primary_result = (uint8_t)(value >> 16);
    detail.cleanup_result = (uint8_t)(value >> 24);
    return detail;
}

static inline bool bladerf_rfic_result_detail_matches(
    const struct bladerf_rfic_result_detail *detail,
    uint16_t transaction_id, uint32_t sequence, uint8_t primary_result)
{
    return detail != 0 && transaction_id != 0 &&
           detail->transaction_id == transaction_id &&
           detail->sequence == sequence &&
           detail->primary_result == primary_result &&
           detail->primary_result != BLADERF_RFIC_RESULT_NO_RESULT &&
           detail->cleanup_result != BLADERF_RFIC_RESULT_NO_RESULT;
}

/* An ACK retry must remain harmless after a later request has been queued.
 * The identity pair prevents it from consuming that later request's result. */
static inline bool bladerf_rfic_result_ack_is_duplicate(
    uint16_t last_acked_transaction_id, uint32_t last_acked_sequence,
    uint16_t transaction_id, uint32_t sequence)
{
    return transaction_id != 0 &&
           last_acked_transaction_id == transaction_id &&
           last_acked_sequence == sequence;
}

static inline bool bladerf_rfic_result_ack_matches(
    bool result_pending, uint8_t queue_count, uint16_t result_transaction_id,
    uint32_t result_sequence, uint16_t ack_transaction_id,
    uint32_t ack_sequence)
{
    return result_pending && queue_count == 0 && ack_transaction_id != 0 &&
           result_transaction_id == ack_transaction_id &&
           result_sequence == ack_sequence;
}

static inline bool bladerf_rfic_tagged_write_allowed(
    uint8_t queue_count, bool result_pending, uint16_t last_result_id)
{
    return queue_count == 0 && (!result_pending || last_result_id == 0);
}

static inline uint8_t bladerf_rfic_result_from_status(int32_t status)
{
    if (status >= 0) {
        return BLADERF_RFIC_RESULT_SUCCESS;
    }
    if (status == INT32_MIN) {
        return BLADERF_RFIC_RESULT_INTERNAL;
    }

    switch (-status) {
        case ETIMEDOUT:
            return BLADERF_RFIC_RESULT_TIMEOUT;
        case EINVAL:
        case ERANGE:
            return BLADERF_RFIC_RESULT_INVALID_PARAMETER;
        case EIO:
            return BLADERF_RFIC_RESULT_IO;
#ifdef EOPNOTSUPP
        case EOPNOTSUPP:
            return BLADERF_RFIC_RESULT_UNSUPPORTED;
#endif
#ifdef ENOTSUP
#if !defined(EOPNOTSUPP) || ENOTSUP != EOPNOTSUPP
        case ENOTSUP:
            return BLADERF_RFIC_RESULT_UNSUPPORTED;
#endif
#endif
        default:
            return BLADERF_RFIC_RESULT_INTERNAL;
    }
}

static inline uint8_t bladerf_rfic_result_latch_error(
    uint8_t latched_result, int32_t status)
{
    if (status >= 0)
        return latched_result;
    if (latched_result != BLADERF_RFIC_RESULT_NO_RESULT &&
        latched_result != BLADERF_RFIC_RESULT_SUCCESS)
        return latched_result;
    return bladerf_rfic_result_from_status(status);
}

/* A callback's boolean return cannot erase a concrete error already latched
 * while it was running. A missing result is still made explicit at terminal
 * publication; a contradictory success/error pair fails closed to the error. */
static inline uint8_t bladerf_rfic_result_finalize(
    uint8_t latched_result, bool operation_succeeded)
{
    if (latched_result == BLADERF_RFIC_RESULT_NO_RESULT)
        return operation_succeeded ? BLADERF_RFIC_RESULT_SUCCESS :
               BLADERF_RFIC_RESULT_INTERNAL;
    if (latched_result == BLADERF_RFIC_RESULT_SUCCESS &&
        !operation_succeeded)
        return BLADERF_RFIC_RESULT_INTERNAL;
    return latched_result;
}

#define BLADERF_RFIC_PROGRESS_SEQUENCE_SHIFT 32
#define BLADERF_RFIC_PROGRESS_SEQUENCE_MASK  0xffffffffu
#define BLADERF_RFIC_PROGRESS_TRANSACTION_SHIFT 16
#define BLADERF_RFIC_PROGRESS_TRANSACTION_MASK  0xffffu
#define BLADERF_RFIC_PROGRESS_COMMAND_SHIFT  8
#define BLADERF_RFIC_PROGRESS_COMMAND_MASK   0xffu
#define BLADERF_RFIC_PROGRESS_STATE_SHIFT    0
#define BLADERF_RFIC_PROGRESS_STATE_MASK     0xffu

/* Values with a recognized class tag identify a live nested RFIC phase. The
 * phase byte is already present in the 64-bit PROGRESS response, so this adds
 * diagnostic meaning without changing packet layout. */
#define BLADERF_RFIC_PROGRESS_PHASE_TAG       0x80u
#define BLADERF_RFIC_PROGRESS_PHASE_CLASS_SHIFT 4
#define BLADERF_RFIC_PROGRESS_PHASE_CLASS_MASK  0x07u
#define BLADERF_RFIC_PROGRESS_PHASE_STEP_MASK   0x0fu

enum bladerf_rfic_progress_phase_class {
    BLADERF_RFIC_PROGRESS_PHASE_INIT = 1,
    BLADERF_RFIC_PROGRESS_PHASE_RFDC = 2,
    BLADERF_RFIC_PROGRESS_PHASE_TXQ = 3,
    BLADERF_RFIC_PROGRESS_PHASE_SAMPLE_RATE = 4,
    BLADERF_RFIC_PROGRESS_PHASE_BANDWIDTH = 5,
    BLADERF_RFIC_PROGRESS_PHASE_RFPLL = 6,
};

static inline uint8_t bladerf_rfic_progress_phase_encode(uint8_t phase_class,
                                                         uint8_t phase_step)
{
    if (phase_class == 0 ||
        phase_class > BLADERF_RFIC_PROGRESS_PHASE_RFPLL ||
        phase_step > BLADERF_RFIC_PROGRESS_PHASE_STEP_MASK) {
        return 0;
    }

    return (uint8_t)(BLADERF_RFIC_PROGRESS_PHASE_TAG |
                     (phase_class <<
                      BLADERF_RFIC_PROGRESS_PHASE_CLASS_SHIFT) |
                     phase_step);
}

static inline bool bladerf_rfic_progress_phase_is_tagged(uint8_t phase)
{
    uint8_t phase_class = (uint8_t)(
        (phase >> BLADERF_RFIC_PROGRESS_PHASE_CLASS_SHIFT) &
        BLADERF_RFIC_PROGRESS_PHASE_CLASS_MASK);

    return (phase & BLADERF_RFIC_PROGRESS_PHASE_TAG) != 0 &&
           phase_class >= BLADERF_RFIC_PROGRESS_PHASE_INIT &&
           phase_class <= BLADERF_RFIC_PROGRESS_PHASE_RFPLL;
}

static inline uint8_t bladerf_rfic_progress_phase_class(uint8_t phase)
{
    return (uint8_t)((phase >> BLADERF_RFIC_PROGRESS_PHASE_CLASS_SHIFT) &
                     BLADERF_RFIC_PROGRESS_PHASE_CLASS_MASK);
}

static inline uint8_t bladerf_rfic_progress_phase_step(uint8_t phase)
{
    return (uint8_t)(phase & BLADERF_RFIC_PROGRESS_PHASE_STEP_MASK);
}

struct bladerf_rfic_progress {
    uint32_t sequence;
    uint16_t transaction_id;
    uint8_t command;
    /* Tagged nested phase while an active entry has one; otherwise the active
     * queue state. When the queue is empty this is the terminal protocol
     * result. The active bit is authoritative in STATUS. */
    uint8_t phase_or_result;
};

static inline uint64_t bladerf_rfic_progress_pack(
    const struct bladerf_rfic_progress *progress)
{
    return ((uint64_t)progress->sequence
            << BLADERF_RFIC_PROGRESS_SEQUENCE_SHIFT) |
           ((uint64_t)progress->transaction_id
            << BLADERF_RFIC_PROGRESS_TRANSACTION_SHIFT) |
           ((uint64_t)progress->command
            << BLADERF_RFIC_PROGRESS_COMMAND_SHIFT) |
           ((uint64_t)progress->phase_or_result
            << BLADERF_RFIC_PROGRESS_STATE_SHIFT);
}

static inline struct bladerf_rfic_progress bladerf_rfic_progress_unpack(
    uint64_t value)
{
    struct bladerf_rfic_progress progress;

    progress.sequence = (uint32_t)(
        (value >> BLADERF_RFIC_PROGRESS_SEQUENCE_SHIFT) &
        BLADERF_RFIC_PROGRESS_SEQUENCE_MASK);
    progress.transaction_id = (uint16_t)(
        (value >> BLADERF_RFIC_PROGRESS_TRANSACTION_SHIFT) &
        BLADERF_RFIC_PROGRESS_TRANSACTION_MASK);
    progress.command = (uint8_t)(
        (value >> BLADERF_RFIC_PROGRESS_COMMAND_SHIFT) &
        BLADERF_RFIC_PROGRESS_COMMAND_MASK);
    progress.phase_or_result = (uint8_t)(
        (value >> BLADERF_RFIC_PROGRESS_STATE_SHIFT) &
        BLADERF_RFIC_PROGRESS_STATE_MASK);
    return progress;
}

#endif
