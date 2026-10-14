/*
 * This file is part of the bladeRF project:
 *   http://www.github.com/nuand/bladeRF
 *
 * Copyright (C) 2018 Nuand LLC
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <libbladeRF.h>

#include "board/board.h"
#include "common.h"
#include "conversions.h"
#include "helpers/have_cap.h"
#include "helpers/timeout.h"
#include "iterators.h"
#include "log.h"
#include "rfic_status_policy.h"
#include "rfic_transaction_policy.h"
#include "rfic_rssi_policy.h"
#include "rfic_progress_policy.h"

// #define BLADERF_HEADLESS_C_DEBUG


/******************************************************************************/
/* Declarations */
/******************************************************************************/

/**
 * @brief       RFIC Command Read
 *
 * Executes a command using the FPGA-based RFIC interface, returning the reply
 * value in *data.
 *
 * @param       dev     Device handle
 * @param[in]   ch      Channel to act upon; use ::BLADERF_CHANNEL_INVALID for
 *                      non-channel-specific commands
 * @param[in]   cmd     Command
 * @param[out]  data    Pointer for response data
 *
 * @return 0 on success, value from \ref RETCODES list on failure
 */
static int _rfic_cmd_read(struct bladerf *dev,
                          bladerf_channel ch,
                          bladerf_rfic_command cmd,
                          uint64_t *data);

/**
 * @brief       RFIC Command Write
 *
 * Executes a command using the FPGA-based RFIC interface, using the supplied
 * data value as an argument. Waits until the command has been executed before
 * returning.
 *
 * @param       dev     Device handle
 * @param[in]   ch      Channel to act upon; use ::BLADERF_CHANNEL_INVALID for
 *                      non-channel-specific commands
 * @param[in]   cmd     Command
 * @param[in]   data    Argument for command
 *
 * @return 0 on success, value from \ref RETCODES list on failure
 */
static int _rfic_cmd_write(struct bladerf *dev,
                           bladerf_channel ch,
                           bladerf_rfic_command cmd,
                           uint64_t data);


/******************************************************************************/
/* Helpers */
/******************************************************************************/

/* Build RFIC address from bladerf_rfic_command and bladerf_channel */
#define RFIC_ADDRESS(cmd, ch) ((cmd & 0xFF) + ((ch & 0xF) << 8))

static int _rfic_fpga_get_status(
    struct bladerf *dev, struct bladerf_rfic_status_register *rfic_status)
{
    uint64_t sreg = 0;
    int status;

    status = _rfic_cmd_read(dev, BLADERF_CHANNEL_INVALID,
                            BLADERF_RFIC_COMMAND_STATUS, &sreg);

    rfic_status->rfic_initialized   = ((sreg >> BLADERF_RFIC_STATUS_INIT_SHIFT) &
                                     BLADERF_RFIC_STATUS_INIT_MASK);
    rfic_status->last_write_success =
        ((sreg >> BLADERF_RFIC_STATUS_WQSUCCESS_SHIFT) &
         BLADERF_RFIC_STATUS_WQSUCCESS_MASK);
    rfic_status->last_write_result_valid =
        ((sreg >> BLADERF_RFIC_STATUS_WQRESULT_VALID_SHIFT) &
         BLADERF_RFIC_STATUS_WQRESULT_VALID_MASK);
    rfic_status->last_write_sequence =
        (uint32_t)((sreg >> BLADERF_RFIC_STATUS_WQSEQUENCE_SHIFT) &
                   BLADERF_RFIC_STATUS_WQSEQUENCE_MASK);
    rfic_status->write_queue_length = ((sreg >> BLADERF_RFIC_STATUS_WQLEN_SHIFT) &
                                     BLADERF_RFIC_STATUS_WQLEN_MASK);
    rfic_status->init_stage = ((sreg >> BLADERF_RFIC_STATUS_STAGE_SHIFT) &
                               BLADERF_RFIC_STATUS_STAGE_MASK);
    rfic_status->active_command_valid = false;
    rfic_status->active_command = 0;
    rfic_status->active_channel = 0;
    rfic_status->active_state = 0;
    if (have_cap_dev(dev, BLADERF_CAP_RFIC_WQ_ACTIVE)) {
        rfic_status->active_command_valid =
            (sreg >> BLADERF_RFIC_STATUS_ACTIVE_VALID_SHIFT) &
            BLADERF_RFIC_STATUS_ACTIVE_VALID_MASK;
        rfic_status->active_command =
            (sreg >> BLADERF_RFIC_STATUS_ACTIVE_CMD_SHIFT) &
            BLADERF_RFIC_STATUS_ACTIVE_CMD_MASK;
        rfic_status->active_channel =
            (sreg >> BLADERF_RFIC_STATUS_ACTIVE_CH_SHIFT) &
            BLADERF_RFIC_STATUS_ACTIVE_CH_MASK;
        rfic_status->active_state =
            (sreg >> BLADERF_RFIC_STATUS_ACTIVE_STATE_SHIFT) &
            BLADERF_RFIC_STATUS_ACTIVE_STATE_MASK;
    }

    return status;
}

static const char *_rfic_fpga_init_stage_name(uint8_t stage)
{
    switch (stage) {
    case BLADERF_RFIC_INIT_STAGE_IDLE: return "idle";
    case BLADERF_RFIC_INIT_STAGE_ENTER: return "enter";
    case BLADERF_RFIC_INIT_STAGE_RFFE_CLEARED: return "rffe-cleared";
    case BLADERF_RFIC_INIT_STAGE_RESET_OUT: return "reset-out";
    case BLADERF_RFIC_INIT_STAGE_ENABLE_SET: return "enable-set";
    case BLADERF_RFIC_INIT_STAGE_AD9361_INIT: return "ad9361-init";
    case BLADERF_RFIC_INIT_STAGE_PHY_OK: return "phy-ok";
    case BLADERF_RFIC_INIT_STAGE_PER_MODULE: return "per-module";
    case BLADERF_RFIC_INIT_STAGE_PER_CHANNEL: return "per-channel";
    case BLADERF_RFIC_INIT_STAGE_DONE: return "done";
    case BLADERF_RFIC_INIT_STAGE_PHY_NULL: return "phy-null";
    case BLADERF_RFIC_INIT_STAGE_PDATA_NULL: return "pdata-null";
    case BLADERF_RFIC_INIT_STAGE_IF_CLOCK_WAIT: return "if-clock-wait";
    case BLADERF_RFIC_INIT_STAGE_IF_CLOCK_OK: return "if-clock-ok";
    case BLADERF_RFIC_INIT_STAGE_IF_CLOCK_TIMEOUT: return "if-clock-timeout";
    case BLADERF_RFIC_INIT_STAGE_CLEANUP_FAILED: return "cleanup-failed";
    case BLADERF_RFIC_INIT_STAGE_DIG_TUNE_POLICY: return "digital-tune-policy";
    default: return "unknown";
    }
}

/* Older firmware has no per-command completion event, so it keeps a bounded
 * compatibility poll budget. Firmware with active transaction reporting is
 * polled until the matching retirement event; this is required because RX
 * RF-DC and TX quadrature calibration can exceed the old 3 ms budget. A
 * separate monotonic deadman returns failure if retirement never arrives; it
 * neither certifies completion nor cancels the still-pending NIOS operation.
 * INIT is also synchronous in NIOS and can take around 1.2 s on the host.
 *
 * Deferred RFIC queue work runs after each request response as well as on
 * idle iterations (bladeRF_nios.c), so status polling does not starve queue
 * progress. A RUNNING queue entry still executes its whole ADI callback
 * synchronously in that deferred-work step; while that call is blocked, the
 * Nios loop cannot service another USB command. */
#define RFIC_SPINWAIT_TRIES        30
#define RFIC_SPINWAIT_DELAY_US     1000
#define RFIC_SPINWAIT_INIT_TRIES   600
#define RFIC_SPINWAIT_INIT_DELAY_US 5000  /* 600 x 5 ms = 3 s */
#define RFIC_SPINWAIT_EVENT_MAX_DELAY_US 20000
#define RFIC_SPINWAIT_ACTIVE_DEADMAN_NS UINT64_C(60000000000)

static int _rfic_cmd_read(struct bladerf *dev, bladerf_channel ch,
                          bladerf_rfic_command cmd, uint64_t *data);
static int _rfic_fpga_get_transaction_status(struct bladerf *dev,
                                             uint16_t *active_id,
                                             uint16_t *last_id,
                                             uint32_t *sequence);
static int _rfic_fpga_get_progress(struct bladerf *dev,
                                   uint16_t *transaction_id,
                                   uint8_t *command,
                                   uint8_t *phase_or_result,
                                   uint32_t *progress_sequence);
static int _rfic_fpga_result_to_status(uint8_t result_code);
static int _rfic_fpga_get_retired_result(struct bladerf *dev,
                                         uint16_t expected_transaction_id,
                                         uint8_t *command,
                                         uint8_t *result_code);
static int _rfic_fpga_get_result_detail(struct bladerf *dev,
                                        uint16_t transaction_id,
                                        uint32_t sequence,
                                        uint8_t primary_result,
                                        uint8_t *cleanup_result);
static int _rfic_fpga_ack_retired_result(struct bladerf *dev,
                                         uint16_t transaction_id,
                                         uint32_t sequence,
                                         bool expected_success);

static int _rfic_fpga_spinwait_for(struct bladerf *dev,
                                   size_t tries,
                                   unsigned int delay_us,
                                   bool sequence_supported,
                                   bool retirement_event_supported,
                                   uint16_t expected_transaction_id,
                                   uint32_t expected_sequence,
                                   bool *retired)
{
    size_t const TRIES       = tries;
    unsigned int poll_delay = delay_us;
    size_t count             = 0;
    int status;
    struct bladerf_rfic_status_register rfic_status;
    uint64_t started_ns = 0;
    uint32_t progress_sequence = 0;
    uint16_t progress_transaction_id = 0;
    uint8_t progress_command = 0;
    uint8_t progress_phase_or_result = 0;
    const bool result_valid_supported = have_cap_dev(
        dev, BLADERF_CAP_RFIC_WQ_RESULT_VALID);
    const bool progress_supported = have_cap_dev(
        dev, BLADERF_CAP_RFIC_PROGRESS);

    if (retired != NULL) {
        *retired = false;
    }

    if (retirement_event_supported &&
        bladerf_monotonic_now_ns(&started_ns) != 0) {
        log_error("%s: monotonic clock unavailable; RFIC command outcome "
                  "remains unresolved\n", __FUNCTION__);
        return BLADERF_ERR_UNEXPECTED;
    }

    /* A successful USB OUT only means that the command packet reached the
     * device. The first status read can race NIOS packet dispatch and still
     * see an empty queue with the previous retirement token. On capable
     * firmware, wait for both an empty queue and this command's exact token.
     * On firmware with active-transaction reporting, retirement is the
     * completion event and the bounded ADI operation owns its own deadline;
     * only the response transport has a timeout. Older firmware retains its
     * finite polling budget. */
    status = BLADERF_ERR_TIMEOUT;
    for (count = 0; retirement_event_supported || count < TRIES; count++) {
        int read_status = _rfic_fpga_get_status(dev, &rfic_status);
        if (read_status < 0) {
            if (sequence_supported) {
                log_error("%s: failed to read NIOS RFIC status while "
                          "waiting for command retirement "
                          "(expected_sequence=%u); the command outcome is "
                          "unknown (%s)\n",
                          __FUNCTION__, expected_sequence,
                          bladerf_strerror(read_status));
            }
            return read_status;
        }

        if (retirement_event_supported) {
            uint16_t active_id;
            uint16_t last_id;
            uint32_t transaction_sequence;

            read_status = _rfic_fpga_get_transaction_status(
                dev, &active_id, &last_id, &transaction_sequence);
            if (read_status < 0) {
                return read_status;
            }

            {
                uint64_t now_ns;

                if (bladerf_monotonic_now_ns(&now_ns) != 0) {
                    log_error("%s: monotonic clock failed while waiting for "
                              "RFIC transaction %u; outcome remains "
                              "unresolved\n", __FUNCTION__,
                              (unsigned int)expected_transaction_id);
                    return BLADERF_ERR_UNEXPECTED;
                }
                if (bladerf2_rfic_deadline_expired(
                        started_ns, now_ns,
                        RFIC_SPINWAIT_ACTIVE_DEADMAN_NS)) {
                    /* Progress is diagnostic only. Read it once on the
                     * failure path rather than adding a third NIOS command
                     * to every exact-retirement poll. */
                    if (progress_supported) {
                        int progress_status = _rfic_fpga_get_progress(
                            dev, &progress_transaction_id,
                            &progress_command, &progress_phase_or_result,
                            &progress_sequence);
                        if (progress_status < 0) {
                            log_debug("%s: progress snapshot unavailable "
                                      "at RFIC deadman expiry (%s)\n",
                                      __FUNCTION__,
                                      bladerf_strerror(progress_status));
                        }
                    }
                    log_warning("%s: failure-only deadman expired for "
                                "RFIC transaction %u (expected_sequence=%u, "
                                "observed_sequence=%u, queue_length=%u, "
                                "active_state=%u, progress_id=%u, "
                                "progress_cmd=0x%02x, progress_phase=0x%02x, "
                                "progress_stage=%s, progress_sequence=%u); "
                                "operation is not "
                                "cancelled and outcome remains unresolved\n",
                                __FUNCTION__,
                                (unsigned int)expected_transaction_id,
                                expected_sequence,
                                rfic_status.last_write_sequence,
                                rfic_status.write_queue_length,
                                (unsigned int)rfic_status.active_state,
                                (unsigned int)progress_transaction_id,
                                (unsigned int)progress_command,
                                (unsigned int)progress_phase_or_result,
                                bladerf_rfic_progress_phase_name(
                                    progress_phase_or_result),
                                progress_sequence);
                    return BLADERF_ERR_TIMEOUT;
                }
            }

            /* STATUS and TRANSACTION_STATUS are separate USB reads. If a
             * retirement landed between them, discard the mixed snapshot. */
            if (transaction_sequence != rfic_status.last_write_sequence) {
                usleep(poll_delay);
                poll_delay = bladerf2_rfic_next_poll_delay_us(
                    poll_delay, RFIC_SPINWAIT_EVENT_MAX_DELAY_US);
                continue;
            }

            if (rfic_status.write_queue_length == 0) {
                if (transaction_sequence != expected_sequence ||
                    active_id != 0 || last_id != expected_transaction_id) {
                    return BLADERF_ERR_UNEXPECTED;
                }
                status = 0;
                break;
            }

            if (active_id != expected_transaction_id ||
                transaction_sequence + 1u != expected_sequence) {
                return BLADERF_ERR_UNEXPECTED;
            }
        } else if (bladerf2_rfic_status_matches_retirement(
                       rfic_status.write_queue_length, sequence_supported,
                       expected_sequence, rfic_status.last_write_sequence)) {
            status = 0;
            break;
        }

        usleep(poll_delay);
        if (retirement_event_supported) {
            poll_delay = bladerf2_rfic_next_poll_delay_us(
                poll_delay, RFIC_SPINWAIT_EVENT_MAX_DELAY_US);
        }
    }

    if (status != 0) {
        if (sequence_supported) {
            log_warning("%s: timed out waiting for RFIC command retirement "
                        "(expected_sequence=%u, observed_sequence=%u, "
                        "queue_length=%u active=%u cmd=0x%02x ch=0x%x "
                        "state=%u progress_id=%u progress_cmd=0x%02x "
                        "progress_phase=0x%02x progress_stage=%s "
                        "progress_sequence=%u)\n",
                        __FUNCTION__, expected_sequence,
                        rfic_status.last_write_sequence,
                        rfic_status.write_queue_length,
                        rfic_status.active_command_valid,
                        rfic_status.active_command,
                        rfic_status.active_channel,
                        rfic_status.active_state,
                        (unsigned int)progress_transaction_id,
                        (unsigned int)progress_command,
                        (unsigned int)progress_phase_or_result,
                        bladerf_rfic_progress_phase_name(
                            progress_phase_or_result),
                        progress_sequence);
        }
        return BLADERF_ERR_TIMEOUT;
    }

    /* The queue drained and this command retired, which is still not the
     * same as the RFIC operation succeeding: NIOS retires failed commands
     * too and publishes their result in WQSUCCESS. */

    /* The separate validity bit distinguishes a retired command result from
     * the 0xFF queue-reset sentinel (whose low bit is one). */
    if (!bladerf2_rfic_status_result_accepted(
            result_valid_supported,
            rfic_status.last_write_result_valid,
            rfic_status.last_write_success,
            sequence_supported, expected_sequence,
            rfic_status.last_write_sequence)) {
        if (result_valid_supported && rfic_status.last_write_result_valid &&
            progress_supported &&
            rfic_status.last_write_sequence == expected_sequence) {
            uint8_t result_command = 0;
            uint8_t result_code = BLADERF_RFIC_RESULT_NO_RESULT;
            int result_status = _rfic_fpga_get_retired_result(
                dev, expected_transaction_id, &result_command, &result_code);

            if (result_status == 0 &&
                result_code != BLADERF_RFIC_RESULT_SUCCESS) {
                uint8_t cleanup_result = BLADERF_RFIC_RESULT_NO_RESULT;
                int detail_status = BLADERF_ERR_UNSUPPORTED;

                if (have_cap_dev(dev, BLADERF_CAP_RFIC_RESULT_DETAIL)) {
                    detail_status = _rfic_fpga_get_result_detail(
                        dev, expected_transaction_id, expected_sequence,
                        result_code, &cleanup_result);
                }

                if (detail_status == 0 &&
                    cleanup_result != BLADERF_RFIC_RESULT_SUCCESS) {
                    log_warning("%s: NIOS RFIC cleanup failed "
                                "(transaction=%u sequence=%u primary=%u "
                                "cleanup=%u)\n",
                                __FUNCTION__,
                                (unsigned int)expected_transaction_id,
                                (unsigned int)expected_sequence,
                                (unsigned int)result_code,
                                (unsigned int)cleanup_result);
                } else if (detail_status < 0 &&
                           have_cap_dev(dev,
                                        BLADERF_CAP_RFIC_RESULT_DETAIL)) {
                    log_warning("%s: NIOS RFIC terminal detail could not be "
                                "correlated (transaction=%u sequence=%u "
                                "status=%d); primary result is preserved\n",
                                __FUNCTION__,
                                (unsigned int)expected_transaction_id,
                                (unsigned int)expected_sequence,
                                detail_status);
                }

                if (result_command == BLADERF_RFIC_COMMAND_INIT &&
                    rfic_status.init_stage ==
                    BLADERF_RFIC_INIT_STAGE_CLEANUP_FAILED) {
                    log_warning("%s: NIOS RFIC command failed and INIT "
                                "cleanup also failed "
                                "(transaction=%u command=0x%02x result=%u, "
                                "init_stage=%u:%s)\n",
                                __FUNCTION__,
                                (unsigned int)expected_transaction_id,
                                (unsigned int)result_command,
                                (unsigned int)result_code,
                                rfic_status.init_stage,
                                _rfic_fpga_init_stage_name(
                                    rfic_status.init_stage));
                } else {
                    log_warning("%s: NIOS RFIC command failed "
                                "(transaction=%u command=0x%02x result=%u)\n",
                                __FUNCTION__,
                                (unsigned int)expected_transaction_id,
                                (unsigned int)result_command,
                                (unsigned int)result_code);
                }
                status = _rfic_fpga_result_to_status(result_code);
                if (have_cap_dev(dev, BLADERF_CAP_RFIC_RESULT_ACK)) {
                    int ack_status = _rfic_fpga_ack_retired_result(
                        dev, expected_transaction_id, expected_sequence,
                        false);
                    if (ack_status < 0) {
                        return ack_status;
                    }
                }
                if (retired != NULL) {
                    *retired = true;
                }
                return status;
            }

            log_warning("%s: NIOS RFIC failure result could not be "
                        "correlated (expected_transaction=%u "
                        "command=0x%02x result=%u read_status=%d)\n",
                        __FUNCTION__,
                        (unsigned int)expected_transaction_id,
                        (unsigned int)result_command,
                        (unsigned int)result_code, result_status);
        }
        if (result_valid_supported &&
            !rfic_status.last_write_result_valid) {
            log_warning("%s: NIOS RFIC queue drained without a retired "
                        "command (initialized=%u, init_stage=%u:%s)\n",
                        __FUNCTION__, rfic_status.rfic_initialized,
                        rfic_status.init_stage,
                        _rfic_fpga_init_stage_name(rfic_status.init_stage));
        } else {
            log_warning("%s: NIOS RFIC command failed "
                        "(initialized=%u, init_stage=%u:%s)\n",
                        __FUNCTION__, rfic_status.rfic_initialized,
                        rfic_status.init_stage,
                        _rfic_fpga_init_stage_name(rfic_status.init_stage));
        }
        return BLADERF_ERR_UNEXPECTED;
    }

    if (have_cap_dev(dev, BLADERF_CAP_RFIC_RESULT_ACK)) {
        status = _rfic_fpga_ack_retired_result(
            dev, expected_transaction_id, expected_sequence, true);
        if (status < 0) {
            return status;
        }
    }
    if (retired != NULL) {
        *retired = true;
    }

    return 0;
}

/* RESULT_DETAIL is an optional extension. Bind it to the already-correlated
 * PROGRESS result before using its secondary cleanup status. */
static int _rfic_fpga_get_result_detail(struct bladerf *dev,
                                        uint16_t transaction_id,
                                        uint32_t sequence,
                                        uint8_t primary_result,
                                        uint8_t *cleanup_result)
{
    uint64_t raw = 0;
    struct bladerf_rfic_result_detail detail;
    int status = _rfic_cmd_read(dev, BLADERF_CHANNEL_INVALID,
                                BLADERF_RFIC_COMMAND_RESULT_DETAIL, &raw);
    if (status < 0) {
        return status;
    }

    detail = bladerf_rfic_result_detail_unpack(raw);
    if (!bladerf_rfic_result_detail_matches(
            &detail, transaction_id, sequence, primary_result)) {
        return BLADERF_ERR_UNEXPECTED;
    }

    *cleanup_result = detail.cleanup_result;
    return 0;
}

static int _rfic_fpga_spinwait(struct bladerf *dev,
                               bool sequence_supported,
                               bool retirement_event_supported,
                               uint16_t expected_transaction_id,
                               uint32_t expected_sequence,
                               bool *retired)
{
    return _rfic_fpga_spinwait_for(dev, RFIC_SPINWAIT_TRIES,
                                   RFIC_SPINWAIT_DELAY_US,
                                   sequence_supported,
                                   retirement_event_supported,
                                   expected_transaction_id,
                                   expected_sequence, retired);
}

static int _rfic_fpga_get_transaction_status(struct bladerf *dev,
                                             uint16_t *active_id,
                                             uint16_t *last_id,
                                             uint32_t *sequence)
{
    uint64_t value;
    int status = _rfic_cmd_read(
        dev, BLADERF_CHANNEL_INVALID,
        BLADERF_RFIC_COMMAND_TRANSACTION_STATUS, &value);
    if (status < 0) {
        return status;
    }

    *active_id = (uint16_t)value;
    *last_id = (uint16_t)(value >> 16);
    *sequence = (uint32_t)(value >> 32);
    return 0;
}

static int _rfic_fpga_get_progress(struct bladerf *dev,
                                   uint16_t *transaction_id,
                                   uint8_t *command,
                                   uint8_t *phase_or_result,
                                   uint32_t *progress_sequence)
{
    uint64_t value;
    struct bladerf_rfic_progress progress;
    int status = _rfic_cmd_read(dev, BLADERF_CHANNEL_INVALID,
                                BLADERF_RFIC_COMMAND_PROGRESS, &value);
    if (status < 0) {
        return status;
    }

    progress = bladerf_rfic_progress_unpack(value);
    *transaction_id = progress.transaction_id;
    *command = progress.command;
    *phase_or_result = progress.phase_or_result;
    *progress_sequence = progress.sequence;
    return 0;
}

static int _rfic_fpga_result_to_status(uint8_t result_code)
{
    switch (result_code) {
        case BLADERF_RFIC_RESULT_TIMEOUT:
            return BLADERF_ERR_TIMEOUT;
        case BLADERF_RFIC_RESULT_INVALID_PARAMETER:
            return BLADERF_ERR_INVAL;
        case BLADERF_RFIC_RESULT_IO:
            return BLADERF_ERR_IO;
        case BLADERF_RFIC_RESULT_UNSUPPORTED:
            return BLADERF_ERR_UNSUPPORTED;
        case BLADERF_RFIC_RESULT_SUCCESS:
            return 0;
        case BLADERF_RFIC_RESULT_INTERNAL:
        case BLADERF_RFIC_RESULT_NO_RESULT:
        default:
            return BLADERF_ERR_UNEXPECTED;
    }
}

/* PROGRESS returns the active command state while work is pending, and the
 * last retired command's protocol result when the queue is empty. Correlate
 * by transaction ID before mapping the result into the stable libbladeRF ABI. */
static int _rfic_fpga_get_retired_result(struct bladerf *dev,
                                         uint16_t expected_transaction_id,
                                         uint8_t *command,
                                         uint8_t *result_code)
{
    uint16_t transaction_id = 0;
    uint32_t sequence = 0;
    int status = _rfic_fpga_get_progress(dev, &transaction_id, command,
                                         result_code, &sequence);

    if (status < 0) {
        return status;
    }
    if (transaction_id != expected_transaction_id ||
        *result_code == BLADERF_RFIC_RESULT_NO_RESULT) {
        return BLADERF_ERR_UNEXPECTED;
    }

    return 0;
}

static int _rfic_fpga_ack_retired_result(struct bladerf *dev,
                                         uint16_t transaction_id,
                                         uint32_t sequence,
                                         bool expected_success)
{
    uint16_t active_id = 0;
    uint16_t last_id = 0;
    uint32_t observed_sequence = 0;
    uint8_t command = 0;
    uint8_t result_code = BLADERF_RFIC_RESULT_NO_RESULT;
    uint32_t progress_sequence = 0;
    int status;

    status = _rfic_fpga_get_transaction_status(
        dev, &active_id, &last_id, &observed_sequence);
    if (status < 0) {
        return status;
    }
    if (active_id != 0 || last_id != transaction_id ||
        observed_sequence != sequence) {
        return BLADERF_ERR_UNEXPECTED;
    }

    status = _rfic_fpga_get_progress(dev, &active_id, &command,
                                     &result_code, &progress_sequence);
    if (status < 0) {
        return status;
    }
    if (active_id != transaction_id ||
        result_code == BLADERF_RFIC_RESULT_NO_RESULT ||
        (result_code == BLADERF_RFIC_RESULT_SUCCESS) != expected_success) {
        return BLADERF_ERR_UNEXPECTED;
    }

    {
        struct bladerf2_board_data *board_data = dev->board_data;
        if (board_data->rfic_wq_pending &&
            board_data->rfic_wq_pending_id == transaction_id) {
            board_data->rfic_wq_pending_result_status =
                _rfic_fpga_result_to_status(result_code);
            board_data->rfic_wq_pending_result_valid = true;
        }
    }

    /* The ACK uses the retired transaction ID as its packet token and the
     * retirement sequence as payload. Firmware accepts an identical retry. */
    return dev->backend->rfic_command_write(
        dev, RFIC_ADDRESS(BLADERF_RFIC_COMMAND_RESULT_ACK,
                          BLADERF_CHANNEL_INVALID),
        sequence, transaction_id);
}

static int _rfic_resolve_pending_write(
    struct bladerf *dev, struct bladerf2_board_data *board_data,
    uint16_t requested_addr, uint64_t requested_data, bool *same_request)
{
    bool retired = false;
    const bool retirement_event_supported = have_cap_dev(
        dev, BLADERF_CAP_RFIC_WQ_ACTIVE);
    struct bladerf_rfic_status_register current_status;
    uint16_t active_id;
    uint16_t last_id;
    uint32_t transaction_sequence;
    int result;

    *same_request = !board_data->rfic_wq_pending_adopted &&
                    board_data->rfic_wq_pending_addr == requested_addr &&
                    board_data->rfic_wq_pending_data == requested_data;

    result = _rfic_fpga_get_transaction_status(
        dev, &active_id, &last_id, &transaction_sequence);
    if (result < 0) {
        return result;
    }

    result = _rfic_fpga_get_status(dev, &current_status);
    if (result < 0) {
        return result;
    }

    /* STATUS and TRANSACTION_STATUS are separate control reads. Never
     * combine fields observed on opposite sides of a retirement/reset edge. */
    if (current_status.last_write_sequence != transaction_sequence) {
        return BLADERF_ERR_WOULD_BLOCK;
    }

    if (current_status.write_queue_length == 0 &&
        transaction_sequence == board_data->rfic_wq_pending_sequence) {
        if (last_id != board_data->rfic_wq_pending_id || active_id != 0) {
            board_data->rfic_wq_pending = false;
            return BLADERF_ERR_UNEXPECTED;
        }
        if (!current_status.last_write_result_valid) {
            if (have_cap_dev(dev, BLADERF_CAP_RFIC_RESULT_ACK) &&
                board_data->rfic_wq_pending_result_valid) {
                result = dev->backend->rfic_command_write(
                    dev, RFIC_ADDRESS(BLADERF_RFIC_COMMAND_RESULT_ACK,
                                      BLADERF_CHANNEL_INVALID),
                    board_data->rfic_wq_pending_sequence,
                    board_data->rfic_wq_pending_id);
                if (result == 0) {
                    board_data->rfic_wq_pending = false;
                    board_data->rfic_wq_pending_result_valid = false;
                }
                return board_data->rfic_wq_pending_result_status;
            }
            board_data->rfic_wq_pending = false;
            return BLADERF_ERR_UNEXPECTED;
        }

        bool result_known = current_status.last_write_success;
        if (current_status.last_write_success) {
            result = 0;
        } else {
            uint8_t result_command = 0;
            uint8_t result_code = BLADERF_RFIC_RESULT_NO_RESULT;
            result = _rfic_fpga_get_retired_result(
                dev, board_data->rfic_wq_pending_id,
                &result_command, &result_code);
            if (result == 0) {
                result_known = true;
                result = result_code == BLADERF_RFIC_RESULT_SUCCESS
                             ? BLADERF_ERR_UNEXPECTED
                             : _rfic_fpga_result_to_status(result_code);
            }
            if (result < 0) {
                log_warning("%s: pending NIOS RFIC command failed or its "
                            "result could not be correlated (transaction=%u "
                            "command=0x%02x result=%u status=%d)\n",
                            __FUNCTION__,
                            (unsigned int)board_data->rfic_wq_pending_id,
                            (unsigned int)result_command,
                            (unsigned int)result_code, result);
            }
        }

        if (have_cap_dev(dev, BLADERF_CAP_RFIC_RESULT_ACK) && result_known) {
            int ack_status = _rfic_fpga_ack_retired_result(
                dev, board_data->rfic_wq_pending_id,
                board_data->rfic_wq_pending_sequence,
                current_status.last_write_success);
            if (ack_status == 0) {
                board_data->rfic_wq_pending = false;
                board_data->rfic_wq_pending_result_valid = false;
            } else if (result == 0) {
                result = ack_status;
            }
        } else {
            board_data->rfic_wq_pending = false;
        }
        return result;
    }

    if (current_status.write_queue_length != 0) {
        if (active_id != board_data->rfic_wq_pending_id ||
            transaction_sequence + 1u !=
                board_data->rfic_wq_pending_sequence) {
            return BLADERF_ERR_WOULD_BLOCK;
        }

        /* The original command is still executing. Re-sending its token here
         * would make the cooperative NIOS hook emit an early duplicate ACK,
         * followed later by the real retirement reply with the same token.
         * Keep one response per request and wait on the authoritative queue
         * retirement record instead. */
        if ((board_data->rfic_wq_pending_addr & 0xffu) ==
            BLADERF_RFIC_COMMAND_INIT) {
            result = _rfic_fpga_spinwait_for(
                dev, RFIC_SPINWAIT_INIT_TRIES,
                RFIC_SPINWAIT_INIT_DELAY_US, true,
                retirement_event_supported,
                board_data->rfic_wq_pending_id,
                board_data->rfic_wq_pending_sequence, &retired);
        } else {
            result = _rfic_fpga_spinwait(
                dev, true, retirement_event_supported,
                board_data->rfic_wq_pending_id,
                board_data->rfic_wq_pending_sequence, &retired);
        }
        if (retired) {
            board_data->rfic_wq_pending = false;
            board_data->rfic_wq_pending_result_valid = false;
        } else if (board_data->rfic_wq_pending_result_valid) {
            result = board_data->rfic_wq_pending_result_status;
        }
        return result;
    } else if (transaction_sequence + 1u !=
               board_data->rfic_wq_pending_sequence) {
        /* A reset or an unrelated writer displaced the only cached result;
         * fail closed instead of attributing a stale result to this request. */
        board_data->rfic_wq_pending = false;
        return BLADERF_ERR_UNEXPECTED;
    } else if (active_id != 0 || last_id == board_data->rfic_wq_pending_id) {
        board_data->rfic_wq_pending = false;
        return BLADERF_ERR_UNEXPECTED;
    }

    /* Re-send the same token and payload. NIOS either recognizes the active
     * or just-retired duplicate, or enqueues it if the first packet never
     * reached firmware. It never executes the same tagged command twice. */
    result = dev->backend->rfic_command_write(
        dev, board_data->rfic_wq_pending_addr,
        board_data->rfic_wq_pending_data, board_data->rfic_wq_pending_id);
    if (result < 0) {
        return result;
    }

    if ((board_data->rfic_wq_pending_addr & 0xffu) ==
        BLADERF_RFIC_COMMAND_INIT) {
        result = _rfic_fpga_spinwait_for(
            dev, RFIC_SPINWAIT_INIT_TRIES, RFIC_SPINWAIT_INIT_DELAY_US,
            true, retirement_event_supported,
            board_data->rfic_wq_pending_id,
            board_data->rfic_wq_pending_sequence, &retired);
    } else {
        result = _rfic_fpga_spinwait(
            dev, true, retirement_event_supported,
            board_data->rfic_wq_pending_id,
            board_data->rfic_wq_pending_sequence, &retired);
    }

    if (retired) {
        board_data->rfic_wq_pending = false;
        board_data->rfic_wq_pending_result_valid = false;
    } else if (board_data->rfic_wq_pending_result_valid) {
        result = board_data->rfic_wq_pending_result_status;
    }
    return result;
}


/******************************************************************************/
/* Low level RFIC Accessors */
/******************************************************************************/

static int _rfic_cmd_read(struct bladerf *dev,
                          bladerf_channel ch,
                          bladerf_rfic_command cmd,
                          uint64_t *data)
{
    return dev->backend->rfic_command_read(dev, RFIC_ADDRESS(cmd, ch), data);
}

static int _rfic_cmd_write(struct bladerf *dev,
                           bladerf_channel ch,
                           bladerf_rfic_command cmd,
                           uint64_t data)
{
    struct bladerf2_board_data *board_data = dev->board_data;
    struct bladerf_rfic_status_register before;
    const bool sequence_supported = have_cap_dev(
        dev, BLADERF_CAP_RFIC_WQ_RESULT_VALID);
    const bool transaction_supported = have_cap_dev(
        dev, BLADERF_CAP_RFIC_WQ_ACTIVE);
    const uint16_t address = RFIC_ADDRESS(cmd, ch);
    uint32_t expected_sequence = 0;
    uint16_t transaction_id = 0;
    uint16_t active_id = 0;
    uint16_t last_id = 0;
    uint32_t transaction_sequence = 0;
    bool retired = false;
    int status;

    /* Resolve a command whose host wait timed out before admitting another
     * write. Retrying the exact payload reuses its transaction token; a late
     * completion is returned to that caller instead of executing twice. */
    if (sequence_supported) {
        CHECK_STATUS(_rfic_fpga_get_status(dev, &before));
        if (transaction_supported && board_data->rfic_wq_pending) {
            bool same_request;
            int previous_result = _rfic_resolve_pending_write(
                dev, board_data, address, data, &same_request);
            if (board_data->rfic_wq_pending) {
                return same_request && previous_result == 0
                           ? 0
                           : (previous_result < 0 ? previous_result
                                                  : BLADERF_ERR_WOULD_BLOCK);
            }
            if (same_request || previous_result < 0) {
                return previous_result;
            }
            CHECK_STATUS(_rfic_fpga_get_status(dev, &before));
        }

        if (transaction_supported) {
            CHECK_STATUS(_rfic_fpga_get_transaction_status(
                dev, &active_id, &last_id, &transaction_sequence));
            if (transaction_sequence != before.last_write_sequence) {
                return BLADERF_ERR_WOULD_BLOCK;
            }
        }

        /* A previous host context may have disconnected after retirement but
         * before reading/ACKing the terminal result. Drain that retained result
         * before issuing this caller's command. A failed orphan is reported
         * first; the new caller may retry its own request after seeing it. */
        if (transaction_supported && before.write_queue_length == 0 &&
            before.last_write_result_valid && last_id != 0 &&
            have_cap_dev(dev, BLADERF_CAP_RFIC_RESULT_ACK)) {
            uint8_t result_command = 0;
            uint8_t result_code = BLADERF_RFIC_RESULT_NO_RESULT;
            int result_status = _rfic_fpga_get_retired_result(
                dev, last_id, &result_command, &result_code);
            if (result_status < 0) {
                return result_status;
            }
            if ((result_code == BLADERF_RFIC_RESULT_SUCCESS) !=
                before.last_write_success) {
                return BLADERF_ERR_UNEXPECTED;
            }
            board_data->rfic_wq_pending = true;
            board_data->rfic_wq_pending_adopted = true;
            board_data->rfic_wq_pending_result_valid = false;
            board_data->rfic_wq_pending_id = last_id;
            board_data->rfic_wq_pending_addr = result_command;
            board_data->rfic_wq_pending_data = 0;
            board_data->rfic_wq_pending_sequence = transaction_sequence;
            result_status = _rfic_fpga_ack_retired_result(
                dev, last_id, transaction_sequence,
                before.last_write_success);
            if (result_status < 0) {
                if (!before.last_write_success &&
                    board_data->rfic_wq_pending_result_valid) {
                    return board_data->rfic_wq_pending_result_status;
                }
                return result_status;
            }
            board_data->rfic_wq_pending = false;
            board_data->rfic_wq_pending_result_valid = false;
            if (!before.last_write_success) {
                log_warning("%s: acknowledged orphaned failed NIOS RFIC "
                            "command (transaction=%u command=0x%02x "
                            "result=%u); current request was not submitted\n",
                            __FUNCTION__, (unsigned int)last_id,
                            (unsigned int)result_command,
                            (unsigned int)result_code);
                return _rfic_fpga_result_to_status(result_code);
            }
        }

        if (before.write_queue_length != 0) {
            /* A new libbladeRF context can outlive the old host context while
             * NIOS is still executing its accepted callback. Adopt exactly
             * one tagged active command and wait for its own retirement
             * before admitting this caller's request. Never guess the old
             * payload or attribute the old result to the new write. */
            bool same_request;
            int previous_result;

            if (!transaction_supported ||
                !bladerf2_rfic_orphan_adoptable(
                    before.write_queue_length, active_id,
                    before.active_command_valid,
                    transaction_sequence == before.last_write_sequence)) {
                return BLADERF_ERR_WOULD_BLOCK;
            }

            board_data->rfic_wq_pending = true;
            board_data->rfic_wq_pending_adopted = true;
            board_data->rfic_wq_pending_result_valid = false;
            board_data->rfic_wq_pending_id = active_id;
            board_data->rfic_wq_pending_addr = RFIC_ADDRESS(
                before.active_command, before.active_channel);
            board_data->rfic_wq_pending_data = 0;
            board_data->rfic_wq_pending_sequence = transaction_sequence + 1u;

            previous_result = _rfic_resolve_pending_write(
                dev, board_data, address, data, &same_request);
            if (board_data->rfic_wq_pending) {
                return previous_result < 0 ? previous_result
                                           : BLADERF_ERR_WOULD_BLOCK;
            }
            if (same_request || previous_result < 0) {
                return previous_result;
            }

            CHECK_STATUS(_rfic_fpga_get_status(dev, &before));
            if (before.write_queue_length != 0) {
                return BLADERF_ERR_WOULD_BLOCK;
            }
            CHECK_STATUS(_rfic_fpga_get_transaction_status(
                dev, &active_id, &last_id, &transaction_sequence));
            if (active_id != 0 ||
                transaction_sequence != before.last_write_sequence) {
                return BLADERF_ERR_WOULD_BLOCK;
            }
        }

        expected_sequence = before.last_write_sequence + 1u;
        if (transaction_supported) {
            transaction_id =
                bladerf2_rfic_next_transaction_id(last_id);
            board_data->rfic_wq_pending = true;
            board_data->rfic_wq_pending_adopted = false;
            board_data->rfic_wq_pending_result_valid = false;
            board_data->rfic_wq_pending_id = transaction_id;
            board_data->rfic_wq_pending_addr = address;
            board_data->rfic_wq_pending_data = data;
            board_data->rfic_wq_pending_sequence = expected_sequence;
        }
    }

    /* Perform the write command. */
    status = dev->backend->rfic_command_write(dev, address, data,
                                             transaction_id);
    if (status < 0) {
        if (transaction_supported &&
            (status == BLADERF_ERR_FPGA_OP ||
             status == BLADERF_ERR_WOULD_BLOCK)) {
            board_data->rfic_wq_pending = false;
            board_data->rfic_wq_pending_adopted = false;
            board_data->rfic_wq_pending_result_valid = false;
        }
        return status;
    }

    /* Wait for this command's retirement event. On firmware that reports the
     * active transaction, completion is not inferred from elapsed time. */
    if (BLADERF_RFIC_COMMAND_INIT == cmd) {
        status = _rfic_fpga_spinwait_for(dev, RFIC_SPINWAIT_INIT_TRIES,
                                         RFIC_SPINWAIT_INIT_DELAY_US,
                                         sequence_supported,
                                         transaction_supported,
                                         transaction_id,
                                         expected_sequence, &retired);
    } else {
        status = _rfic_fpga_spinwait(dev, sequence_supported,
                                     transaction_supported,
                                     transaction_id,
                                     expected_sequence, &retired);
    }

    if (transaction_supported && retired) {
        board_data->rfic_wq_pending = false;
        board_data->rfic_wq_pending_result_valid = false;
    } else if (transaction_supported &&
               board_data->rfic_wq_pending_result_valid &&
               board_data->rfic_wq_pending_id == transaction_id) {
        /* The RFIC outcome is known even if the result-ACK response was lost.
         * Preserve that outcome for this caller and retry the idempotent ACK
         * before any subsequent tagged operation. */
        status = board_data->rfic_wq_pending_result_status;
    }
    return status;
}


/******************************************************************************/
/* Initialization */
/******************************************************************************/

static bool _rfic_fpga_is_present(struct bladerf *dev)
{
    uint64_t sreg = 0;
    int status;

    status = _rfic_cmd_read(dev, BLADERF_CHANNEL_INVALID,
                            BLADERF_RFIC_COMMAND_STATUS, &sreg);
    if (status < 0) {
        return false;
    }

    return true;
}

static int _rfic_fpga_get_init_state(struct bladerf *dev,
                                     bladerf_rfic_init_state *state)
{
    uint64_t data;

    CHECK_STATUS(_rfic_cmd_read(dev, BLADERF_CHANNEL_INVALID,
                                BLADERF_RFIC_COMMAND_INIT, &data));

    *state = (bladerf_rfic_init_state)data;

    return 0;
}

static bool _rfic_fpga_is_initialized(struct bladerf *dev)
{
    bladerf_rfic_init_state state;
    int status;

    status = _rfic_fpga_get_init_state(dev, &state);
    if (status < 0) {
        log_error("%s: failed to get RFIC initialization state: %s\n",
                  __FUNCTION__, bladerf_strerror(status));
        return false;
    }

    return BLADERF_RFIC_INIT_STATE_ON == state;
}

static bool _rfic_fpga_is_standby(struct bladerf *dev)
{
    bladerf_rfic_init_state state;
    int status;

    status = _rfic_fpga_get_init_state(dev, &state);
    if (status < 0) {
        log_error("%s: failed to get RFIC initialization state: %s\n",
                  __FUNCTION__, bladerf_strerror(status));
        return false;
    }

    return BLADERF_RFIC_INIT_STATE_STANDBY == state;
}

static int _rfic_fpga_initialize(struct bladerf *dev)
{
    log_debug("%s: initializing\n", __FUNCTION__);

    return _rfic_cmd_write(dev, BLADERF_CHANNEL_INVALID,
                           BLADERF_RFIC_COMMAND_INIT,
                           BLADERF_RFIC_INIT_STATE_ON);
}

static int _rfic_fpga_standby(struct bladerf *dev)
{
    log_debug("%s: entering standby mode\n", __FUNCTION__);

    return _rfic_cmd_write(dev, BLADERF_CHANNEL_INVALID,
                           BLADERF_RFIC_COMMAND_INIT,
                           BLADERF_RFIC_INIT_STATE_STANDBY);
}

static int _rfic_fpga_deinitialize(struct bladerf *dev)
{
    log_debug("%s: deinitializing\n", __FUNCTION__);

    return _rfic_cmd_write(dev, BLADERF_CHANNEL_INVALID,
                           BLADERF_RFIC_COMMAND_INIT,
                           BLADERF_RFIC_INIT_STATE_OFF);
}


/******************************************************************************/
/* Enable */
/******************************************************************************/

static int _rfic_fpga_enable_module(struct bladerf *dev,
                                    bladerf_channel ch,
                                    bool ch_enable)
{
    struct bladerf2_board_data *board_data = dev->board_data;
    struct controller_fns const *rfic      = board_data->rfic;
    bladerf_direction dir = BLADERF_CHANNEL_IS_TX(ch) ? BLADERF_TX : BLADERF_RX;
    uint32_t reg;     /* RFFE register value */
    bool ch_enabled;  /* Channel: initial state */
    bool ch_pending;  /* Channel: target state is not initial state */
    bool dir_enabled; /* Direction: initial state */
    bool dir_enable;  /* Direction: target state */
    bool dir_pending; /* Direction: target state is not initial state */
    bool be_toggle;   /* Backend: toggle off/on to reset backend FIFO */
    bool be_teardown; /* Backend: disable backend module */
    bool be_setup;    /* Backend: enable backend module */

    /* Read RFFE control register */
    CHECK_STATUS(dev->backend->rffe_control_read(dev, &reg));

#ifdef BLADERF_HEADLESS_C_DEBUG
    uint32_t reg_old = reg;
#endif

    /* Calculate initial and target states */
    ch_enabled  = _rffe_ch_enabled(reg, ch);
    dir_enabled = _rffe_dir_enabled(reg, dir);
    dir_enable  = ch_enable || _rffe_dir_otherwise_enabled(reg, ch);
    ch_pending  = ch_enabled != ch_enable;
    dir_pending = dir_enabled != dir_enable;
    be_toggle   = !BLADERF_CHANNEL_IS_TX(ch) && ch_enable && !dir_pending;
    be_setup    = be_toggle || (dir_pending && dir_enable);
    be_teardown = be_toggle || (dir_pending && !dir_enable);

    /* Perform Direction Teardown */
    if (dir_pending && !dir_enable) {
        sync_deinit(&board_data->sync[dir]);
        perform_format_deconfig(dev, dir);
    }

    /* Perform Channel Setup/Teardown */
    if (ch_pending) {
        /* Set/unset TX mute */
        if (BLADERF_CHANNEL_IS_TX(ch)) {
            CHECK_STATUS(rfic->set_txmute(dev, ch, !ch_enable));
        }

        /* Execute RFIC enable command. */
        CHECK_STATUS(_rfic_cmd_write(dev, ch, BLADERF_RFIC_COMMAND_ENABLE,
                                     ch_enable ? 1 : 0));
    }

    /* Perform backend teardown */
    if (be_teardown) {
        CHECK_STATUS(dev->backend->enable_module(dev, dir, false));
    }

    /* Perform backend setup */
    if (be_setup) {
        if (dir == BLADERF_RX) {
            struct bladerf_sync *sync = &board_data->sync[BLADERF_RX];

            if (sync->initialized) {
                CHECK_STATUS(sync_prime_stream(sync, 0));
            }
        }

        CHECK_STATUS(dev->backend->enable_module(dev, dir, true));
    }

    /* Warn the user if the sample rate isn't reasonable */
    if (ch_pending && ch_enable) {
        check_total_sample_rate(dev);
    }

#ifdef BLADERF_HEADLESS_C_DEBUG
    /* Debug output... */
    if (BLADERF_LOG_LEVEL_VERBOSE == log_get_verbosity()) {
        CHECK_STATUS(dev->backend->rffe_control_read(dev, &reg));

        log_verbose(
            "%s: %s ch[en=%d->%d pend=%d] dir[en=%d->%d pend=%d] be[clr=%d "
            "su=%d td=%d] reg=0x%08x->0x%08x\n",
            __FUNCTION__, channel2str(ch), ch_enabled, ch_enable, ch_pending,
            dir_enabled, dir_enable, dir_pending, be_toggle, be_setup,
            be_teardown, reg_old, reg);
    }
#endif

    return 0;
}


/******************************************************************************/
/* Sample rate */
/******************************************************************************/

static int _rfic_fpga_get_sample_rate(struct bladerf *dev,
                                      bladerf_channel ch,
                                      bladerf_sample_rate *rate)
{
    uint64_t readval;

    CHECK_STATUS(
        _rfic_cmd_read(dev, ch, BLADERF_RFIC_COMMAND_SAMPLERATE, &readval));

    *rate = (uint32_t)readval;

    return 0;
}

static int _rfic_fpga_set_sample_rate(struct bladerf *dev,
                                      bladerf_channel ch,
                                      bladerf_sample_rate rate)
{
    return _rfic_cmd_write(dev, ch, BLADERF_RFIC_COMMAND_SAMPLERATE, rate);
}


/******************************************************************************/
/* Frequency */
/******************************************************************************/

static int _rfic_fpga_get_frequency(struct bladerf *dev,
                                    bladerf_channel ch,
                                    bladerf_frequency *frequency)
{
    bladerf_frequency freq;

    CHECK_STATUS(
        _rfic_cmd_read(dev, ch, BLADERF_RFIC_COMMAND_FREQUENCY, &freq));

    *frequency = freq;

    return 0;
}

static int _rfic_fpga_set_frequency(struct bladerf *dev,
                                    bladerf_channel ch,
                                    bladerf_frequency frequency)
{
    struct bladerf_range const *range = NULL;

    CHECK_STATUS(dev->board->get_frequency_range(dev, ch, &range));

    if (!is_within_range(range, frequency)) {
        return BLADERF_ERR_RANGE;
    }

    return _rfic_cmd_write(dev, ch, BLADERF_RFIC_COMMAND_FREQUENCY, frequency);
}

static int _rfic_fpga_select_band(struct bladerf *dev,
                                  bladerf_channel ch,
                                  bladerf_frequency frequency)
{
    // This functionality is unnecessary with the headless architecture.
    return 0;
}


/******************************************************************************/
/* Bandwidth */
/******************************************************************************/

static int _rfic_fpga_get_bandwidth(struct bladerf *dev,
                                    bladerf_channel ch,
                                    bladerf_bandwidth *bandwidth)
{
    uint64_t readval;

    CHECK_STATUS(
        _rfic_cmd_read(dev, ch, BLADERF_RFIC_COMMAND_BANDWIDTH, &readval));

    *bandwidth = (uint32_t)readval;

    return 0;
}

static int _rfic_fpga_set_bandwidth(struct bladerf *dev,
                                    bladerf_channel ch,
                                    bladerf_bandwidth bandwidth,
                                    bladerf_bandwidth *actual)
{
    struct bladerf2_board_data *board_data = dev->board_data;
    struct controller_fns const *rfic      = board_data->rfic;
    struct bladerf_range const *range      = NULL;

    CHECK_STATUS(dev->board->get_bandwidth_range(dev, ch, &range));

    if (!is_within_range(range, bandwidth)) {
        return BLADERF_ERR_RANGE;
    }

    CHECK_STATUS(
        _rfic_cmd_write(dev, ch, BLADERF_RFIC_COMMAND_BANDWIDTH, bandwidth));

    if (actual != NULL) {
        return rfic->get_bandwidth(dev, ch, actual);
    }

    return 0;
}


/******************************************************************************/
/* Gain */
/* These functions handle offset */
/******************************************************************************/

static int _rfic_fpga_get_gain(struct bladerf *dev,
                               bladerf_channel ch,
                               int *gain)
{
    struct bladerf2_board_data *board_data = dev->board_data;
    struct controller_fns const *rfic      = board_data->rfic;
    char const *stage = BLADERF_CHANNEL_IS_TX(ch) ? "dsa" : "full";
    int val;
    float offset;

    CHECK_STATUS(get_gain_offset(dev, ch, &offset));

    CHECK_STATUS(rfic->get_gain_stage(dev, ch, stage, &val));

    *gain = __round_int(val + offset);

    return 0;
}

static int _rfic_fpga_set_gain(struct bladerf *dev,
                               bladerf_channel ch,
                               int gain)
{
    struct bladerf2_board_data *board_data = dev->board_data;
    struct controller_fns const *rfic      = board_data->rfic;
    char const *stage = BLADERF_CHANNEL_IS_TX(ch) ? "dsa" : "full";
    float offset;

    CHECK_STATUS(get_gain_offset(dev, ch, &offset));

    return rfic->set_gain_stage(dev, ch, stage, __round_int(gain - offset));
}


/******************************************************************************/
/* Gain mode */
/******************************************************************************/

static int _rfic_fpga_get_gain_mode(struct bladerf *dev,
                                    bladerf_channel ch,
                                    bladerf_gain_mode *mode)
{
    uint64_t readval;

    if (BLADERF_CHANNEL_IS_TX(ch)) {
        log_warning("%s: automatic gain control not valid for TX channels\n",
                    __FUNCTION__);
        *mode = BLADERF_GAIN_DEFAULT;
        return 0;
    }

    CHECK_STATUS(
        _rfic_cmd_read(dev, ch, BLADERF_RFIC_COMMAND_GAINMODE, &readval));

    *mode = (bladerf_gain_mode)readval;

    return 0;
}

static int _rfic_fpga_set_gain_mode(struct bladerf *dev,
                                    bladerf_channel ch,
                                    bladerf_gain_mode mode)
{
    if (BLADERF_CHANNEL_IS_TX(ch)) {
        log_debug("%s: gain-control modes are unsupported on TX channels\n",
                  __FUNCTION__);
        return BLADERF_ERR_UNSUPPORTED;
    }

    return _rfic_cmd_write(dev, ch, BLADERF_RFIC_COMMAND_GAINMODE, mode);
}


/******************************************************************************/
/* Gain stage */
/* These functions handle scaling */
/******************************************************************************/

static int _rfic_fpga_get_gain_stage(struct bladerf *dev,
                                     bladerf_channel ch,
                                     char const *stage,
                                     int *gain)
{
    struct bladerf_range const *range = NULL;
    uint64_t val;

    if ((BLADERF_CHANNEL_IS_TX(ch) && strcmp(stage, "dsa") != 0) ||
        (!BLADERF_CHANNEL_IS_TX(ch) && strcmp(stage, "full") != 0)) {
        log_error("%s: unknown gain stage '%s'\n", __FUNCTION__, stage);
        return BLADERF_ERR_INVAL;
    }

    CHECK_STATUS(dev->board->get_gain_stage_range(dev, ch, stage, &range));

    CHECK_STATUS(_rfic_cmd_read(dev, ch, BLADERF_RFIC_COMMAND_GAIN, &val));

    *gain = __unscale_int(range, val);

    if (BLADERF_CHANNEL_IS_TX(ch)) {
        *gain = -(*gain);
    }

    return 0;
}

static int _rfic_fpga_set_gain_stage(struct bladerf *dev,
                                     bladerf_channel ch,
                                     char const *stage,
                                     int gain)
{
    struct bladerf_range const *range = NULL;
    int64_t val;

    if ((BLADERF_CHANNEL_IS_TX(ch) && strcmp(stage, "dsa") != 0) ||
        (!BLADERF_CHANNEL_IS_TX(ch) && strcmp(stage, "full") != 0)) {
        log_error("%s: unknown gain stage '%s'\n", __FUNCTION__, stage);
        return BLADERF_ERR_INVAL;
    }

    CHECK_STATUS(dev->board->get_gain_stage_range(dev, ch, stage, &range));

    if (BLADERF_CHANNEL_IS_TX(ch) && gain < -89) {
        val = -89750;
    } else {
        val = __scale_int64(range, clamp_to_range(range, gain));
    }

    if (BLADERF_CHANNEL_IS_TX(ch)) {
        val = -val;
    }

    return _rfic_cmd_write(dev, ch, BLADERF_RFIC_COMMAND_GAIN, (uint64_t)val);
}


/******************************************************************************/
/* RSSI */
/******************************************************************************/

static int _rfic_fpga_get_rssi(struct bladerf *dev,
                               bladerf_channel ch,
                               int *pre_rssi,
                               int *sym_rssi)
{
    uint64_t readval;
    int16_t mult, pre, sym;

    CHECK_STATUS(_rfic_cmd_read(dev, ch, BLADERF_RFIC_COMMAND_RSSI, &readval));

    mult = (int16_t)((readval >> BLADERF_RFIC_RSSI_MULT_SHIFT) &
                     BLADERF_RFIC_RSSI_MULT_MASK);
    pre  = (int16_t)((readval >> BLADERF_RFIC_RSSI_PRE_SHIFT) &
                    BLADERF_RFIC_RSSI_PRE_MASK);
    sym  = (int16_t)((readval >> BLADERF_RFIC_RSSI_SYM_SHIFT) &
                    BLADERF_RFIC_RSSI_SYM_MASK);

    if (!bladerf2_rssi_multiplier_valid(mult)) {
        return BLADERF_ERR_UNEXPECTED;
    }

    *pre_rssi = __round_int(pre / (float)mult);
    *sym_rssi = __round_int(sym / (float)mult);

    return 0;
}


/******************************************************************************/
/* Filter */
/******************************************************************************/

static int _rfic_fpga_get_filter(struct bladerf *dev,
                                 bladerf_channel ch,
                                 bladerf_rfic_rxfir *rxfir,
                                 bladerf_rfic_txfir *txfir)
{
    uint64_t readval;

    CHECK_STATUS(
        _rfic_cmd_read(dev, ch, BLADERF_RFIC_COMMAND_FILTER, &readval));

    if (BLADERF_CHANNEL_IS_TX(ch)) {
        if (NULL != txfir) {
            *txfir = (bladerf_rfic_txfir)readval;
        } else {
            return BLADERF_ERR_INVAL;
        }
    } else {
        if (NULL != rxfir) {
            *rxfir = (bladerf_rfic_rxfir)readval;
        } else {
            return BLADERF_ERR_INVAL;
        }
    }

    return 0;
}

static int _rfic_fpga_set_filter(struct bladerf *dev,
                                 bladerf_channel ch,
                                 bladerf_rfic_rxfir rxfir,
                                 bladerf_rfic_txfir txfir)
{
    uint64_t data;

    if (BLADERF_CHANNEL_IS_TX(ch)) {
        data = (uint64_t)txfir;
    } else {
        data = (uint64_t)rxfir;
    }

    return _rfic_cmd_write(dev, ch, BLADERF_RFIC_COMMAND_FILTER, data);
}


/******************************************************************************/
/* TX Mute */
/******************************************************************************/

static int _rfic_fpga_get_txmute(struct bladerf *dev,
                                 bladerf_channel ch,
                                 bool *state)
{
    if (BLADERF_CHANNEL_IS_TX(ch)) {
        uint64_t readval;

        if (state == NULL) {
            return BLADERF_ERR_INVAL;
        }

        CHECK_STATUS(
            _rfic_cmd_read(dev, ch, BLADERF_RFIC_COMMAND_TXMUTE, &readval));

        *state = readval > 0;
        return 0;
    }

    return BLADERF_ERR_UNSUPPORTED;
}

static int _rfic_fpga_set_txmute(struct bladerf *dev,
                                 bladerf_channel ch,
                                 bool state)
{
    if (BLADERF_CHANNEL_IS_TX(ch)) {
        return _rfic_cmd_write(dev, ch, BLADERF_RFIC_COMMAND_TXMUTE,
                               state ? 1 : 0);
    }

    return BLADERF_ERR_UNSUPPORTED;
}


/******************************************************************************/
/* Fastlock */
/******************************************************************************/

static int _rfic_fpga_store_fastlock_profile(struct bladerf *dev,
                                             bladerf_channel ch,
                                             uint32_t profile)
{
    return _rfic_cmd_write(dev, ch, BLADERF_RFIC_COMMAND_FASTLOCK, profile);
}


/******************************************************************************/
/* Function pointers */
/******************************************************************************/

struct controller_fns const rfic_fpga_control = {
    FIELD_INIT(.is_present, _rfic_fpga_is_present),
    FIELD_INIT(.is_standby, _rfic_fpga_is_standby),
    FIELD_INIT(.is_initialized, _rfic_fpga_is_initialized),
    FIELD_INIT(.get_init_state, _rfic_fpga_get_init_state),

    FIELD_INIT(.initialize, _rfic_fpga_initialize),
    FIELD_INIT(.standby, _rfic_fpga_standby),
    FIELD_INIT(.deinitialize, _rfic_fpga_deinitialize),

    FIELD_INIT(.enable_module, _rfic_fpga_enable_module),

    FIELD_INIT(.get_sample_rate, _rfic_fpga_get_sample_rate),
    FIELD_INIT(.set_sample_rate, _rfic_fpga_set_sample_rate),

    FIELD_INIT(.get_frequency, _rfic_fpga_get_frequency),
    FIELD_INIT(.set_frequency, _rfic_fpga_set_frequency),
    FIELD_INIT(.select_band, _rfic_fpga_select_band),

    FIELD_INIT(.get_bandwidth, _rfic_fpga_get_bandwidth),
    FIELD_INIT(.set_bandwidth, _rfic_fpga_set_bandwidth),

    FIELD_INIT(.get_gain_mode, _rfic_fpga_get_gain_mode),
    FIELD_INIT(.set_gain_mode, _rfic_fpga_set_gain_mode),

    FIELD_INIT(.get_gain, _rfic_fpga_get_gain),
    FIELD_INIT(.set_gain, _rfic_fpga_set_gain),

    FIELD_INIT(.get_gain_stage, _rfic_fpga_get_gain_stage),
    FIELD_INIT(.set_gain_stage, _rfic_fpga_set_gain_stage),

    FIELD_INIT(.get_rssi, _rfic_fpga_get_rssi),

    FIELD_INIT(.get_filter, _rfic_fpga_get_filter),
    FIELD_INIT(.set_filter, _rfic_fpga_set_filter),

    FIELD_INIT(.get_txmute, _rfic_fpga_get_txmute),
    FIELD_INIT(.set_txmute, _rfic_fpga_set_txmute),

    FIELD_INIT(.store_fastlock_profile, _rfic_fpga_store_fastlock_profile),

    FIELD_INIT(.command_mode, RFIC_COMMAND_FPGA),
};
