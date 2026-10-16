/* This file is part of the bladeRF project:
 *   http://www.github.com/nuand/bladeRF
 *
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

#include "devices.h"

#include <errno.h>
#include <string.h>
#include <sys/alt_timestamp.h>

#if defined(BOARD_BLADERF_MICRO) && defined(BLADERF_NIOS_LIBAD936X)

#include "devices_rfic.h"
#include "ad936x_helpers.h"
#include "nios_pkt_8x64.h"
#include "nios_pkt_retune2.h"
#include "rfic_frequency_policy.h"
#include "rfic_scheduler_policy.h"


/******************************************************************************/
/* Global state */
/******************************************************************************/

static struct rfic_state state;

static bool rfic_latch_calibration_fault(void);

static int32_t rfic_checked_poll_status(int32_t status)
{
    if (status > 1 && state.phy != NULL) {
        ad9361_mark_calibration_state_unknown(state.phy);
    }
    return rfic_retained_poll_status(status);
}

bool rfic_frequency_state_available(void)
{
    return state.phy != NULL && !state.calibration_faulted;
}

bool rfic_phy_present(void)
{
    return state.phy != NULL;
}

bool rfic_command_state_faulted(void)
{
    return state.calibration_faulted;
}

/* Preserve the first concrete failure for the currently executing queue
 * callback. Cleanup may fail afterwards, but must not overwrite the error
 * that caused cleanup to run. */
void rfic_command_record_error(int32_t status)
{
    struct rfic_queue_entry *active = rfic_queue_peek(&state.write_queue);

    if (status < 0 && active != NULL &&
        active->state == ENTRY_STATE_RUNNING) {
        if (state.init_timing.active &&
            state.init_timing.primary_error_status == 0) {
            state.init_timing.primary_error_status = status;
            state.init_timing.error_stage = state.init_stage;
            state.init_timing.error_tail_phase = state.init_tail_phase;
            state.init_timing.error_module_index = state.init_module_index;
            state.init_timing.error_pending_mask =
                (state.rfdc_job_pending ? 0x01u : 0u) |
                (state.tx_quad_job_pending ? 0x02u : 0u) |
                (state.sample_rate_job_pending ? 0x04u : 0u) |
                (state.bandwidth_job_pending ? 0x08u : 0u) |
                (state.init_job_pending ? 0x10u : 0u) |
                (state.init_tail_pending ? 0x20u : 0u) |
                (state.rfpll_job_pending ? 0x40u : 0u) |
                (state.fir_enable_job_pending ? 0x80u : 0u);
        }
        active->result_code = bladerf_rfic_result_latch_error(
            active->result_code, status);
    }
}

void rfic_command_record_cleanup_error(int32_t status)
{
    struct rfic_queue_entry *active = rfic_queue_peek(&state.write_queue);

    if (status < 0 && active != NULL &&
        active->state == ENTRY_STATE_RUNNING &&
        active->cleanup_result_code == BLADERF_RFIC_RESULT_SUCCESS) {
        active->cleanup_result_code = bladerf_rfic_result_from_status(status);
    }
}

bool rfic_frequency_peer_unknown(bladerf_module target)
{
    bool tx_target;
    bool dependent_fdd;

    if (target != BLADERF_MODULE_RX && target != BLADERF_MODULE_TX) {
        return true;
    }

    if (state.phy == NULL || state.phy->pdata == NULL) {
        return true;
    }

    tx_target = target == BLADERF_MODULE_TX;
    dependent_fdd = state.phy->pdata->fdd &&
                    !state.phy->pdata->fdd_independent_mode;

    return rfic_dependent_fdd_peer_frequency_unknown(
        dependent_fdd, tx_target,
        !state.frequency_invalid[BLADERF_MODULE_RX],
        !state.frequency_invalid[BLADERF_MODULE_TX]);
}


/******************************************************************************/
/* Helpers */
/******************************************************************************/

void rfic_invalidate_frequency(bladerf_module module)
{
    state.frequency_invalid[module] = true;
    if (state.phy != NULL &&
        (module == BLADERF_MODULE_RX || module == BLADERF_MODULE_TX)) {
        ad9361_fastlock_invalidate_current_lo(
            state.phy, module == BLADERF_MODULE_TX);
    }
}

bool rfic_read_frequency_live(bladerf_module module, uint8_t profile,
                              uint64_t *frequency_hz,
                              uint8_t *failure_stage,
                              bool *txq_pending)
{
    int32_t status;
    bool tx;

    if (frequency_hz == NULL || state.phy == NULL ||
        (module != BLADERF_MODULE_RX && module != BLADERF_MODULE_TX)) {
        return false;
    }
    if (failure_stage != NULL) {
        *failure_stage = NIOS_PKT_RETUNE2_FAILURE_NONE;
    }
    if (txq_pending != NULL) {
        *txq_pending = false;
    }

    tx = module == BLADERF_MODULE_TX;
    /* NIOS loads fastlock banks directly over RFFE SPI, outside the ADI
     * fastlock cache. A previous RFIC FASTLOCK_STORE can therefore leave
     * cached bytes for this slot that no longer match hardware. Force the
     * readback below to decode the profile that was actually recalled. */
    ad9361_fastlock_invalidate_cache(state.phy, tx);
    status = ad9361_fastlock_sync_current_lo(state.phy, tx, profile, 0,
                                             frequency_hz);
    if (status != 0) {
        state.frequency_invalid[module] = true;
        return false;
    }

    /* Fastlock recall bypasses the ordinary LO setter, so TX must run the
     * same final-output calibration gate after port selection and readback. */
    if (tx) {
        if (txq_pending == NULL ||
            !rfic_start_tx_quad_calibration_for_retune(
                txq_pending, failure_stage)) {
            return false;
        }
        if (*txq_pending)
            return true;
    }

    /* profile_activate() calls this only after the RFPLL lock, AD9361 port,
     * and RFFE SPDT operations have all succeeded. This is the commit point
     * for the FPGA tuning owner's cached LO state. */
    state.frequency_invalid[module] = false;
    return true;
}

void rfic_fail_tx_quad_calibration(int32_t status,
                                   uint8_t *failure_stage)
{
    RFIC_ERR("TX quadrature calibration", status);
    rfic_command_record_cleanup_error(state.job_context.tx_quad_job.cleanup_status);
    state.tx_quad_job_pending = false;
    state.tx_quad_commit_tx_frequency = false;
    state.tx_quad_resume_command = false;
    state.tx_quad_retune_owner = false;
    if (failure_stage != NULL) {
        *failure_stage = NIOS_PKT_RETUNE2_FAILURE_TX_QUAD_CALIBRATION;
    }
    state.frequency_invalid[BLADERF_MODULE_TX] = true;
    if (state.phy != NULL) {
        ad9361_fastlock_invalidate_current_lo(state.phy, true);
    }
    if (state.phy != NULL &&
        ad9361_calibration_state_unknown(state.phy)) {
        (void)rfic_latch_calibration_fault();
    } else if (!rfic_disable_tx_output()) {
        rfic_command_record_cleanup_error(-EIO);
        RFIC_ERR("TX RFFE fail-closed disconnect", -EIO);
    }
}

bool rfic_start_tx_quad_calibration_for_frequency(bool *pending,
                                                   uint8_t *failure_stage)
{
    int32_t status;

    if (pending == NULL || state.phy == NULL) {
        rfic_fail_tx_quad_calibration(-EIO, failure_stage);
        return false;
    }
    *pending = false;
    status = ad9361_ensure_tx_quad_calib_start(state.phy,
                                                &state.job_context.tx_quad_job);
    if (status == 1) {
        return true;
    }
    if (status < 0) {
        rfic_fail_tx_quad_calibration(status, failure_stage);
        return false;
    }

    state.tx_quad_job_pending = true;
    state.tx_quad_commit_tx_frequency = true;
    state.tx_quad_resume_command = false;
    state.tx_quad_retune_owner = false;
    state.frequency_invalid[BLADERF_MODULE_TX] = true;
    *pending = true;
    return true;
}

bool rfic_start_tx_quad_calibration_for_command(bool *pending,
                                                 uint8_t *failure_stage)
{
    int32_t status;

    if (pending == NULL || state.phy == NULL) {
        rfic_fail_tx_quad_calibration(-EIO, failure_stage);
        return false;
    }
    *pending = false;
    status = ad9361_ensure_tx_quad_calib_start(state.phy,
                                                &state.job_context.tx_quad_job);
    if (status == 1) {
        return true;
    }
    if (status < 0) {
        rfic_fail_tx_quad_calibration(status, failure_stage);
        return false;
    }

    state.tx_quad_job_pending = true;
    state.tx_quad_commit_tx_frequency = false;
    state.tx_quad_resume_command = true;
    state.tx_quad_retune_owner = false;
    *pending = true;
    return true;
}

bool rfic_start_tx_quad_calibration_for_retune(bool *pending,
                                                uint8_t *failure_stage)
{
    int32_t status;

    if (pending == NULL || state.phy == NULL) {
        rfic_fail_tx_quad_calibration(-EIO, failure_stage);
        return false;
    }
    *pending = false;
    status = ad9361_ensure_tx_quad_calib_start(state.phy,
                                                &state.job_context.tx_quad_job);
    if (status == 1) {
        state.frequency_invalid[BLADERF_MODULE_TX] = false;
        return true;
    }
    if (status < 0) {
        rfic_fail_tx_quad_calibration(status, failure_stage);
        return false;
    }

    state.tx_quad_job_pending = true;
    state.tx_quad_commit_tx_frequency = false;
    state.tx_quad_resume_command = false;
    state.tx_quad_retune_owner = true;
    state.frequency_invalid[BLADERF_MODULE_TX] = true;
    *pending = true;
    return true;
}

int32_t rfic_poll_tx_quad_calibration_for_retune(uint8_t *failure_stage)
{
    int32_t status;

    if (!state.tx_quad_job_pending || !state.tx_quad_retune_owner ||
        state.phy == NULL) {
        return -EINVAL;
    }
    status = ad9361_tx_quad_calibration_transaction_poll(
        state.phy, &state.job_context.tx_quad_job);
    status = rfic_checked_poll_status(status);
    if (status == 1) {
        return 1;
    }
    if (status == 0 && ad9361_calibration_state_unknown(state.phy)) {
        status = -EIO;
    }

    state.tx_quad_job_pending = false;
    state.tx_quad_retune_owner = false;
    if (status < 0) {
        rfic_fail_tx_quad_calibration(status, failure_stage);
        return status;
    }

    state.frequency_invalid[BLADERF_MODULE_TX] = false;
    return 0;
}

bool rfic_tx_quad_retune_owner(void)
{
    return state.tx_quad_job_pending && state.tx_quad_retune_owner;
}

/******************************************************************************/
/* Dispatching */
/******************************************************************************/

/**
 * @brief      Function pointers for RFIC command dispatching
 */
struct rfic_command_fns {
    bool (*write64)(struct rfic_state *, bladerf_channel, uint64_t);
    bool (*write32)(struct rfic_state *, bladerf_channel, uint32_t);
    bool (*read64)(struct rfic_state *, bladerf_channel, uint64_t *);
    bool (*read32)(struct rfic_state *, bladerf_channel, uint32_t *);
    command_bitmask bitmask;
    uint8_t command;
};

/**
 * Function pointers for command dispatching
 */
struct rfic_command_fns const funcs[] = {
    // clang-format off
    {
        FIELD_INIT(.command, BLADERF_RFIC_COMMAND_STATUS),
        FIELD_INIT(.read64, _rfic_cmd_rd_status),
        FIELD_INIT(.bitmask, RFIC_CMD_CHAN_SYSTEM),
    },
    {
        FIELD_INIT(.command, BLADERF_RFIC_COMMAND_TRANSACTION_STATUS),
        FIELD_INIT(.read64, _rfic_cmd_rd_transaction_status),
        FIELD_INIT(.bitmask, RFIC_CMD_CHAN_SYSTEM),
    },
    {
        FIELD_INIT(.command, BLADERF_RFIC_COMMAND_PROGRESS),
        FIELD_INIT(.read64, _rfic_cmd_rd_progress),
        FIELD_INIT(.bitmask, RFIC_CMD_CHAN_SYSTEM),
    },
    {
        FIELD_INIT(.command, BLADERF_RFIC_COMMAND_RESULT_DETAIL),
        FIELD_INIT(.read64, _rfic_cmd_rd_result_detail),
        FIELD_INIT(.bitmask, RFIC_CMD_CHAN_SYSTEM),
    },
    {
        FIELD_INIT(.command, BLADERF_RFIC_COMMAND_INIT),
        FIELD_INIT(.write32, _rfic_cmd_wr_init),
        FIELD_INIT(.read32, _rfic_cmd_rd_init),
        FIELD_INIT(.bitmask, RFIC_CMD_CHAN_SYSTEM),
    },
    {
        FIELD_INIT(.command, BLADERF_RFIC_COMMAND_ENABLE),
        FIELD_INIT(.write32, _rfic_cmd_wr_enable),
        FIELD_INIT(.read32, _rfic_cmd_rd_enable),
        FIELD_INIT(.bitmask,
            RFIC_CMD_INIT_REQD | RFIC_CMD_CHAN_TX | RFIC_CMD_CHAN_RX),
    },
    {
        FIELD_INIT(.command, BLADERF_RFIC_COMMAND_SAMPLERATE),
        FIELD_INIT(.write32, _rfic_cmd_wr_samplerate),
        FIELD_INIT(.read32, _rfic_cmd_rd_samplerate),
        FIELD_INIT(.bitmask,
            RFIC_CMD_INIT_REQD | RFIC_CMD_CHAN_TX | RFIC_CMD_CHAN_RX),
    },
    {
        FIELD_INIT(.command, BLADERF_RFIC_COMMAND_FREQUENCY),
        FIELD_INIT(.write64, _rfic_cmd_wr_frequency),
        FIELD_INIT(.read64, _rfic_cmd_rd_frequency),
        FIELD_INIT(.bitmask,
            RFIC_CMD_INIT_REQD | RFIC_CMD_CHAN_TX | RFIC_CMD_CHAN_RX),
    },
    {
        FIELD_INIT(.command, BLADERF_RFIC_COMMAND_BANDWIDTH),
        FIELD_INIT(.write32, _rfic_cmd_wr_bandwidth),
        FIELD_INIT(.read32, _rfic_cmd_rd_bandwidth),
        FIELD_INIT(.bitmask,
            RFIC_CMD_INIT_REQD | RFIC_CMD_CHAN_TX | RFIC_CMD_CHAN_RX),
    },
    {
        FIELD_INIT(.command, BLADERF_RFIC_COMMAND_GAINMODE),
        FIELD_INIT(.write32, _rfic_cmd_wr_gainmode),
        FIELD_INIT(.read32, _rfic_cmd_rd_gainmode),
        FIELD_INIT(.bitmask, RFIC_CMD_INIT_REQD | RFIC_CMD_CHAN_RX),
    },
    {
        FIELD_INIT(.command, BLADERF_RFIC_COMMAND_GAIN),
        FIELD_INIT(.write32, _rfic_cmd_wr_gain),
        FIELD_INIT(.read32, _rfic_cmd_rd_gain),
        FIELD_INIT(.bitmask,
            RFIC_CMD_INIT_REQD | RFIC_CMD_CHAN_TX | RFIC_CMD_CHAN_RX),
    },
    {
        FIELD_INIT(.command, BLADERF_RFIC_COMMAND_RSSI),
        FIELD_INIT(.read64, _rfic_cmd_rd_rssi),
        FIELD_INIT(.bitmask,
            RFIC_CMD_INIT_REQD | RFIC_CMD_CHAN_TX | RFIC_CMD_CHAN_RX),
    },
    {
        FIELD_INIT(.command, BLADERF_RFIC_COMMAND_FILTER),
        FIELD_INIT(.write32, _rfic_cmd_wr_filter),
        FIELD_INIT(.read32, _rfic_cmd_rd_filter),
        FIELD_INIT(.bitmask,
            RFIC_CMD_INIT_REQD | RFIC_CMD_CHAN_TX | RFIC_CMD_CHAN_RX),
    },
    {
        FIELD_INIT(.command, BLADERF_RFIC_COMMAND_TXMUTE),
        FIELD_INIT(.write32, _rfic_cmd_wr_txmute),
        FIELD_INIT(.read32, _rfic_cmd_rd_txmute),
        FIELD_INIT(.bitmask, RFIC_CMD_INIT_REQD | RFIC_CMD_CHAN_TX),
    },
    {
        FIELD_INIT(.command, BLADERF_RFIC_COMMAND_FASTLOCK),
        FIELD_INIT(.write32, _rfic_cmd_wr_fastlock),
        FIELD_INIT(.bitmask,
            RFIC_CMD_INIT_REQD | RFIC_CMD_CHAN_TX | RFIC_CMD_CHAN_RX),
    },
    // clang-format on
};

/**
 * @brief      Retrieve command pointer for a given command
 *
 * @param[in]  command  The command
 *
 * @return     The command pointer.
 */
static struct rfic_command_fns const *_get_cmd_ptr(uint8_t command)
{
    for (size_t i = 0; i < ARRAY_SIZE(funcs); ++i) {
        if (command == funcs[i].command) {
            return &funcs[i];
        }
    }

    return NULL;
}

/**
 * @brief      Determines if bitmask contains a given bit.
 *
 * @param[in]  bitmask  The bitmask
 * @param[in]  bit      The bit
 *
 * @return     True if bitmask has bit, False otherwise.
 */
static bool _has_bit(command_bitmask bitmask, command_bitmask bit)
{
    return ((bitmask & bit) > 0);
}

/**
 * @brief      Check validity of command pointer in current state
 *
 * @param      cmd_ptr  The command pointer
 * @param[in]  channel  The channel
 *
 * @return     True if the current state is valid, False otherwise.
 */
static bool _check_validity(struct rfic_command_fns const *cmd_ptr,
                            bladerf_channel channel)
{
    bool is_system;
    bool is_tx;
    bool is_rx;

    if (!_rfic_chan_is_valid(channel)) {
        DBG("%s: invalid RFIC channel 0x%x\n", __FUNCTION__,
            (unsigned int)channel);
        return false;
    }

    is_system = _rfic_chan_is_system(channel);
    is_tx = !is_system && BLADERF_CHANNEL_IS_TX(channel);
    is_rx = !is_system && !BLADERF_CHANNEL_IS_TX(channel);

    /* System channel check */
    if (is_system && !_has_bit(cmd_ptr->bitmask, RFIC_CMD_CHAN_SYSTEM)) {
        DBG("%s: operation not valid for wildcard channel\n", __FUNCTION__);
        return false;
    }

    /* Initialization check */
    if (!_rfic_is_initialized(&state) &&
        _has_bit(cmd_ptr->bitmask, RFIC_CMD_INIT_REQD)) {
        DBG("%s: operation not valid in uninitialized state\n", __FUNCTION__);
        return false;
    }

    /* TX channel check */
    if (is_tx && !_has_bit(cmd_ptr->bitmask, RFIC_CMD_CHAN_TX)) {
        DBG("%s: operation not valid for TX channels\n", __FUNCTION__);
        return false;
    }

    /* RX channel check */
    if (is_rx && !_has_bit(cmd_ptr->bitmask, RFIC_CMD_CHAN_RX)) {
        DBG("%s: operation not valid for RX channels\n", __FUNCTION__);
        return false;
    }

    /* All tests passed */
    return true;
}

/**
 * @brief      Write queue worker
 *
 * Sets e->rv to 0xFE if there is no write handler for the command.
 *
 * @param      q     rfic_queue pointer
 */
static bool rfic_latch_calibration_fault(void)
{
    if (state.phy == NULL ||
        !ad9361_calibration_state_unknown(state.phy)) {
        return false;
    }

    state.calibration_faulted = true;
    state.init_state = BLADERF_RFIC_INIT_STATE_OFF;
    if (!rfic_force_rf_path_off()) {
        rfic_command_record_cleanup_error(-EIO);
        DBG("%s: failed to disconnect RFFE after unknown calibration state\n",
            __FUNCTION__);
    }
    return true;
}

static void rfic_command_work_wq(struct rfic_queue *q)
{
    struct rfic_queue_entry *e = rfic_queue_peek(q);

    if (e == NULL && state.phy == NULL && !state.init_job_started) {
        /* A consumed/absent PHY cannot own a live ADI continuation. Retire
         * stale scheduler flags left behind after queue recovery; otherwise
         * they keep host-owned SPI fenced forever with no callback to poll. */
        state.init_job_pending = false;
        state.init_job_started = false;
        state.init_job_status = 0;
        state.init_tail_phase = RFIC_INIT_TAIL_IDLE;
        state.init_tail_pending = false;
        state.init_module_index = 0;
        state.init_channel_index = 0;
        state.init_restore_standby = false;
        state.init_nested_complete = false;
        state.init_nested_status = 0;
        state.rfpll_job_pending = false;
        state.frequency_rfpll_resume_command = false;
        state.frequency_rfpll_status = 0;
        state.sample_rate_job_pending = false;
        state.sample_rate_resume_command = false;
        state.bandwidth_job_pending = false;
        state.bandwidth_resume_command = false;
        state.rfdc_job_pending = false;
        state.tx_quad_job_pending = false;
        state.tx_quad_commit_tx_frequency = false;
        state.tx_quad_retune_owner = false;
        state.tx_quad_resume_command = false;
        state.fir_enable_job_pending = false;
        state.fir_enable_resume_command = false;
        state.fir_enable_init_pair_resume = false;
        return;
    }

    /* A RETUNE2 entry owns the shared TXQ context until its terminal poll.
     * Do not let a newly queued RFIC write steal that poll or inherit its
     * result; the scheduler routes polling back to the owning RETUNE2 queue. */
    if (state.tx_quad_retune_owner) {
        return;
    }

    if (NULL == e) {
        return;
    }

#if 0
    if (ENTRY_STATE_COMPLETE == e->state)
#endif
    {
        RFIC_DBG("WRW", _rfic_cmdstr(e->cmd), _rfic_chanstr(e->ch),
                 _rfic_statestr(e->state), -1, e->value);
    }

    switch (e->state) {
        case ENTRY_STATE_NEW: {
            e->state = ENTRY_STATE_RUNNING;
            if (e->cmd == BLADERF_RFIC_COMMAND_INIT &&
                e->value == BLADERF_RFIC_INIT_STATE_ON &&
                state.init_state != BLADERF_RFIC_INIT_STATE_ON) {
                const uint64_t now = (uint64_t)alt_timestamp();
                memset(&state.init_timing, 0, sizeof(state.init_timing));
                state.init_timing.active = true;
                state.init_timing.transaction_id = e->transaction_id;
                state.init_timing.start_ticks = now;
                state.init_timing.last_worker_ticks = now;
                state.init_timing.have_worker_tick = true;
            }
            q->progress_sequence++;
            break;
        }

        case ENTRY_STATE_RUNNING: {
            struct rfic_command_fns const *f = _get_cmd_ptr(e->cmd);
            int32_t step_status;

            e->rv    = false;

            if (state.init_job_pending) {
                step_status = ad9361_init_transaction_poll(&state.init_job);
                step_status = rfic_checked_poll_status(step_status);
                if (step_status == 1) {
                    q->progress_sequence++;
                    break;
                }

                state.init_job_pending = false;
                state.init_job_status = step_status;
                state.init_job_stage = (uint8_t)state.init_job.stage;
                state.init_setup_stage =
                    (uint8_t)state.init_job.setup_job.stage;
                state.init_job_cleanup_status =
                    state.init_job.cleanup_status;
                if (step_status < 0)
                    rfic_command_record_error(step_status);
                else {
                    /* INIT's retained constructor and the ordered tail jobs
                     * intentionally share one bounded context. Start the
                     * first tail job from a clean context: ADI transaction
                     * objects may be terminal but still carry stale child
                     * flags that their next start() correctly rejects as
                     * already active. The scalar completion report above is
                     * all the constructor state the continuation needs. */
                    memset(&state.job_context, 0,
                           sizeof(state.job_context));
                }

                /* Re-enter the INIT callback on the next worker pass. It
                 * performs the common success continuation or fail-closed
                 * cleanup; terminal queue retirement stays centralized. */
                q->progress_sequence++;
                break;
            } else if (state.fir_enable_job_pending) {
                step_status = ad9361_fir_enable_transaction_poll(
                    state.phy, &state.job_context.fir_enable_job);
                step_status = rfic_checked_poll_status(step_status);
                if (step_status == 1) {
                    q->progress_sequence++;
                    break;
                }

                rfic_command_record_cleanup_error(
                    state.job_context.fir_enable_job.cleanup_status);
                state.fir_enable_job_pending = false;
                if (step_status < 0) {
                    rfic_command_record_error(step_status);
                    state.fir_enable_resume_command = false;
                    if (e->cmd == BLADERF_RFIC_COMMAND_INIT) {
                        state.init_nested_status = step_status;
                        state.init_nested_complete = true;
                        q->progress_sequence++;
                        break;
                    }
                } else {
                    /* The next callback pass publishes the filter cache (or
                     * advances the INIT tail) without replaying coefficient
                     * writes or starting another calibration. */
                    if (e->cmd == BLADERF_RFIC_COMMAND_INIT)
                        state.fir_enable_init_pair_resume = true;
                    else
                        state.fir_enable_resume_command = true;
                    q->progress_sequence++;
                    break;
                }
            } else if (state.rfpll_job_pending) {
                step_status = ad9361_rfpll_clock_transaction_poll(
                    state.phy, &state.job_context.rfpll_job);
                step_status = rfic_checked_poll_status(step_status);
                if (step_status == 1) {
                    q->progress_sequence++;
                    break;
                }

                state.rfpll_job_pending = false;
                state.frequency_rfpll_status = step_status;
                state.frequency_rfpll_resume_command = true;
                if (step_status < 0)
                    rfic_command_record_error(step_status);

                /* Re-enter the same frequency callback after lock/readback.
                 * It resumes at RFDC/TXQ without reprogramming the LO. */
                q->progress_sequence++;
                break;
            } else if (state.sample_rate_job_pending) {
                step_status = ad9361_sample_rate_transaction_poll(
                    state.phy, &state.job_context.sample_rate_job);
                step_status = rfic_checked_poll_status(step_status);
                if (step_status == 1) {
                    q->progress_sequence++;
                    break;
                }

                rfic_command_record_cleanup_error(
                    state.job_context.sample_rate_job.cleanup_status);
                rfic_command_record_cleanup_error(
                    state.job_context.sample_rate_job.bandwidth_job.cleanup_status);
                rfic_command_record_cleanup_error(
                    state.job_context.sample_rate_job.bandwidth_job.
                        tracking_restore_status);
                state.sample_rate_job_pending = false;
                if (step_status < 0) {
                    if (rfic_sample_rate_restore_cache_after_rollback(
                            state.job_context.sample_rate_job.rollback_complete,
                            state.phy->current_bw_valid,
                            !ad9361_calibration_state_unknown(state.phy))) {
                        /* The old clock plan and bandwidth completed their
                         * rollback transaction. Restore the cache-validity
                         * snapshot from before the rejected rate change. */
                        memcpy(state.samplerate_invalid,
                               state.sample_rate_saved_samplerate_invalid,
                               sizeof(state.samplerate_invalid));
                        memcpy(state.bandwidth_invalid,
                               state.sample_rate_saved_bandwidth_invalid,
                               sizeof(state.bandwidth_invalid));
                    }
                    rfic_command_record_error(step_status);
                    if (e->cmd == BLADERF_RFIC_COMMAND_INIT) {
                        state.init_nested_status = step_status;
                        state.init_nested_complete = true;
                        q->progress_sequence++;
                        break;
                    }
                } else {
                    /* The sample-rate transaction has committed clock,
                     * filters, TXQ and restoration. Resume the command only
                     * for its post-commit interface check/cache publication. */
                    state.sample_rate_resume_command = true;
                    q->progress_sequence++;
                    break;
                }
            } else if (state.bandwidth_job_pending) {
                step_status = ad9361_rf_bandwidth_transaction_poll(
                    state.phy, &state.job_context.bandwidth_job);
                step_status = rfic_checked_poll_status(step_status);
                if (step_status == 1) {
                    q->progress_sequence++;
                    break;
                }

                rfic_command_record_cleanup_error(
                    state.job_context.bandwidth_job.cleanup_status);
                rfic_command_record_cleanup_error(
                    state.job_context.bandwidth_job.tracking_restore_status);
                state.bandwidth_job_pending = false;
                if (step_status < 0) {
                    rfic_command_record_error(step_status);
                    if (e->cmd == BLADERF_RFIC_COMMAND_INIT) {
                        state.init_nested_status = step_status;
                        state.init_nested_complete = true;
                        q->progress_sequence++;
                        break;
                    }
                } else {
                    /* Resume only the wrapper's cache publication. The
                     * hardware/filter/TXQ transaction has already finished. */
                    state.bandwidth_resume_command = true;
                    q->progress_sequence++;
                    break;
                }
            } else if (state.tx_quad_job_pending) {
                enum rfic_calibration_poll_action action;

                step_status = ad9361_tx_quad_calibration_transaction_poll(
                    state.phy, &state.job_context.tx_quad_job);
                step_status = rfic_checked_poll_status(step_status);
                action = rfic_calibration_poll_action(
                    step_status, state.tx_quad_resume_command);
                if (action == RFIC_CALIBRATION_POLL_AGAIN) {
                    q->progress_sequence++;
                    break;
                }

                rfic_command_record_cleanup_error(
                    state.job_context.tx_quad_job.cleanup_status);
                state.tx_quad_job_pending = false;
                if (e->cmd == BLADERF_RFIC_COMMAND_INIT) {
                    if (step_status == 0 &&
                        ad9361_calibration_state_unknown(state.phy))
                        step_status = -EIO;
                    state.tx_quad_resume_command = false;
                    if (step_status < 0) {
                        rfic_command_record_error(step_status);
                        rfic_fail_tx_quad_calibration(step_status, NULL);
                    } else if (state.tx_quad_commit_tx_frequency) {
                        state.frequency_invalid[BLADERF_MODULE_TX] = false;
                    }
                    state.tx_quad_commit_tx_frequency = false;
                    state.init_nested_status = step_status;
                    state.init_nested_complete = true;
                    q->progress_sequence++;
                    break;
                }
                if (action == RFIC_CALIBRATION_POLL_FAIL_COMMAND) {
                    state.tx_quad_resume_command = false;
                    rfic_command_record_error(step_status);
                    rfic_fail_tx_quad_calibration(step_status, NULL);
                } else {
                    if (state.tx_quad_commit_tx_frequency)
                        state.frequency_invalid[BLADERF_MODULE_TX] = false;
                    if (action == RFIC_CALIBRATION_POLL_RESUME_COMMAND) {
                        if (rfic_latch_calibration_fault()) {
                            rfic_command_record_error(-EIO);
                            state.tx_quad_resume_command = false;
                            e->rv = false;
                        } else {
                            /* Keep the command RUNNING. Its callback resumes
                             * at the saved commit phase, without replaying
                             * the RFIC setup that preceded calibration. */
                            state.tx_quad_commit_tx_frequency = false;
                            q->progress_sequence++;
                            break;
                        }
                    }
                    if (e->result_code != BLADERF_RFIC_RESULT_IO)
                        e->rv = true;
                }
                state.tx_quad_commit_tx_frequency = false;
            } else if (state.rfdc_job_pending) {
                enum rfic_rfdc_poll_action action;

                step_status = ad9361_rfdc_calibration_poll(
                    state.phy, &state.job_context.rfdc_job);
                step_status = rfic_checked_poll_status(step_status);
                action = rfic_rfdc_poll_action(
                    step_status, state.rfdc_resume_enable_command);
                if (action == RFIC_RFDC_POLL_INVALID_STATUS) {
                    step_status = -EIO;
                    ad9361_mark_calibration_state_unknown(state.phy);
                    action = rfic_rfdc_poll_action(
                        step_status, state.rfdc_resume_enable_command);
                }
                if (action == RFIC_RFDC_POLL_AGAIN) {
                    q->progress_sequence++;
                    break;
                }

                rfic_command_record_cleanup_error(
                    state.job_context.rfdc_job.cleanup_status);
                state.rfdc_job_pending = false;
                if (e->cmd == BLADERF_RFIC_COMMAND_INIT) {
                    if (step_status == 0 &&
                        ad9361_calibration_state_unknown(state.phy))
                        step_status = -EIO;
                    if (step_status < 0) {
                        rfic_command_record_error(step_status);
                    } else {
                        state.last_rfdc_calib_freq[BLADERF_RX]
                                                    [state.rfdc_job_port] =
                            state.rfdc_job_frequency;
                        if (state.rfdc_commit_rx_frequency)
                            state.frequency_invalid[BLADERF_MODULE_RX] = false;
                    }
                    state.rfdc_commit_rx_frequency = false;
                    state.init_nested_status = step_status;
                    state.init_nested_complete = true;
                    q->progress_sequence++;
                    break;
                }
                if (action == RFIC_RFDC_POLL_RESUME_RX_ENABLE) {
                    state.rfdc_resume_enable_status = step_status;
                    if (step_status < 0) {
                        rfic_command_record_error(step_status);
                    } else {
                        state.last_rfdc_calib_freq[BLADERF_RX]
                                                    [state.rfdc_job_port] =
                            state.rfdc_job_frequency;
                    }
                    /* Re-enter only the saved RX-enable continuation, which
                     * performs its RFFE commit or fail-closed cleanup. */
                    q->progress_sequence++;
                    break;
                }
                if (step_status < 0) {
                    rfic_command_record_error(step_status);
                } else {
                    state.last_rfdc_calib_freq[BLADERF_RX]
                                                [state.rfdc_job_port] =
                        state.rfdc_job_frequency;
                    if (state.rfdc_commit_rx_frequency)
                        state.frequency_invalid[BLADERF_MODULE_RX] = false;
                    e->rv = true;
                }
                state.rfdc_commit_rx_frequency = false;
            } else {
                /* State may have changed after enqueue (for example,
                 * INIT OFF can run before a command accepted while
                 * initialized). Revalidate immediately before touching
                 * the PHY and tolerate stale/corrupt command IDs. */
                if (NULL != f && _check_validity(f, e->ch) &&
                    (NULL != f->write64 || NULL != f->write32)) {
                    if (!rfic_calibration_command_allowed(
                            state.calibration_faulted,
                            e->cmd == BLADERF_RFIC_COMMAND_INIT)) {
                        rfic_command_record_error(-EIO);
                    } else {
                        e->rv = false;
                        if (NULL != f->write64) {
                            e->rv = f->write64(&state, e->ch, e->value);
                        } else if (_rfic_write32_payload_valid(e->value)) {
                            e->rv = f->write32(&state, e->ch,
                                               (uint32_t)e->value);
                        }
                    }
                }

                /* A resumable callback leaves the queue head RUNNING. The
                 * only permitted follow-up work is its matching poll and
                 * the read-only progress/status service hook. */
                if (rfic_scheduler_calibration_pending(
                        state.rfdc_job_pending, state.tx_quad_job_pending,
                        state.sample_rate_job_pending,
                        state.bandwidth_job_pending,
                        state.init_job_pending || state.init_tail_pending ||
                        state.rfpll_job_pending ||
                        state.fir_enable_job_pending)) {
                    q->progress_sequence++;
                    break;
                }
            }

            /* A completed response bit is not enough if the AD9361 still
             * reports an in-flight calibration. Poison the entire RFIC
             * command surface and disconnect the board paths immediately. */
            if (rfic_latch_calibration_fault()) {
                e->rv = false;
                rfic_command_record_error(-EIO);
            }

            /* Preserve the first concrete callback/poll error even if a
             * later wrapper accidentally reports success. A false callback
             * with no recorded errno remains an explicit INTERNAL result. */
            e->result_code = bladerf_rfic_result_finalize_with_cleanup(
                e->result_code, e->rv, e->cleanup_result_code);
            e->rv = e->result_code == BLADERF_RFIC_RESULT_SUCCESS;

            /* Publish COMPLETE only after the full ADI callback returns. */
            e->state = ENTRY_STATE_COMPLETE;
            if (e->cmd == BLADERF_RFIC_COMMAND_INIT &&
                state.init_timing.active &&
                state.init_timing.transaction_id == e->transaction_id) {
                state.init_timing.total_ticks =
                    (uint64_t)alt_timestamp() - state.init_timing.start_ticks;
                state.init_timing.retirement_sequence =
                    q->last_rv_sequence + 1u;
                state.init_timing.init_stage = state.init_stage;
                state.init_timing.tail_phase = state.init_tail_phase;
                state.init_timing.module_index = state.init_module_index;
                state.init_timing.result_code = e->result_code;
                state.init_timing.cleanup_result_code =
                    e->cleanup_result_code;
                state.init_timing.adi_status = state.init_job_status;
                state.init_timing.adi_cleanup_status =
                    state.init_job_cleanup_status;
                state.init_timing.adi_stage = state.init_job_stage;
                state.init_timing.setup_stage = state.init_setup_stage;
                state.init_timing.active = false;
                state.init_timing.valid = true;
            }
            q->progress_sequence++;

            break;
        }

        case ENTRY_STATE_COMPLETE: {
            /* Drop the item from the queue */
            q->last_rv = e->rv;
            q->last_rv_valid = true;
            q->last_rv_transaction_id = e->transaction_id;
            q->last_rv_ch = e->ch;
            q->last_rv_cmd = e->cmd;
            q->last_rv_value = e->value;
            q->last_result_code = e->result_code;
            q->last_cleanup_result_code = e->cleanup_result_code;
            q->last_rv_sequence++;
            rfic_dequeue(q, NULL);
            break;
        }

        default:
            break;
    }
}

static int rfic_find_transaction(struct rfic_queue *q, uint16_t transaction_id,
                                 uint8_t ch, uint8_t cmd, uint64_t value)
{
    size_t i;

    if (transaction_id == 0) {
        return 0;
    }

    for (i = 0; i < q->count; i++) {
        size_t idx = (q->rem_idx + i) & (COMMAND_QUEUE_MAX - 1);
        struct rfic_queue_entry const *e = &q->entries[idx];

        if (e->transaction_id == transaction_id) {
            return e->ch == ch && e->cmd == cmd && e->value == value ? 1 : -1;
        }
    }

    if (q->last_rv_transaction_id == transaction_id) {
        return q->last_rv_ch == ch && q->last_rv_cmd == cmd &&
                       q->last_rv_value == value
                   ? 1
                   : -1;
    }

    return 0;
}

bool rfic_command_write(uint16_t addr, uint64_t data)
{
    return rfic_command_write_with_transaction_id(addr, data, 0);
}

bool rfic_command_transaction_is_duplicate(uint16_t addr, uint64_t data,
                                           uint16_t transaction_id)
{
    if (transaction_id == 0 || !_rfic_address_fields_valid(addr)) {
        return false;
    }

    return rfic_find_transaction(&state.write_queue, transaction_id,
                                 _rfic_unpack_chan(addr),
                                 _rfic_unpack_cmd(addr), data) == 1;
}

bool rfic_command_ack_result(uint16_t transaction_id, uint32_t sequence)
{
    struct rfic_queue *q = &state.write_queue;

    if (transaction_id == 0) {
        return false;
    }

    /* ACK retries are safe if the first ACK took effect but its USB response
     * was lost. Bind the idempotence record to both transaction and sequence. */
    if (bladerf_rfic_result_ack_is_duplicate(
            q->last_acked_transaction_id, q->last_acked_sequence,
            transaction_id, sequence)) {
        return true;
    }

    if (!bladerf_rfic_result_ack_matches(
            q->last_rv_valid, q->count, q->last_rv_transaction_id,
            q->last_rv_sequence, transaction_id, sequence) ||
        q->last_result_code == BLADERF_RFIC_RESULT_NO_RESULT) {
        return false;
    }

    q->last_rv_valid = false;
    q->last_result_code = BLADERF_RFIC_RESULT_NO_RESULT;
    q->last_cleanup_result_code = BLADERF_RFIC_RESULT_NO_RESULT;
    q->last_acked_transaction_id = transaction_id;
    q->last_acked_sequence = sequence;
    return true;
}

bool rfic_command_write_with_transaction_id(uint16_t addr, uint64_t data,
                                            uint16_t transaction_id)
{
    if (!_rfic_address_fields_valid(addr)) {
        return false;
    }

    uint8_t cmd                      = _rfic_unpack_cmd(addr);
    uint8_t const ch                 = _rfic_unpack_chan(addr);
    struct rfic_command_fns const *f = _get_cmd_ptr(cmd);
    bool rv                          = false;
    bool clear_legacy_result         = false;

    if (cmd == BLADERF_RFIC_COMMAND_RESULT_ACK) {
        if ((data >> 32) != 0 || transaction_id == 0) {
            return false;
        }
        return rfic_command_ack_result(transaction_id, (uint32_t)data);
    }

    if (!rfic_calibration_command_allowed(
            state.calibration_faulted,
            cmd == BLADERF_RFIC_COMMAND_INIT)) {
        return false;
    }

    /* Once a host opts into tagged stop-and-wait transactions, accepting a
     * legacy untagged write would make its terminal result unacknowledgeable. */
    if (transaction_id == 0 &&
        state.write_queue.tagged_protocol_enabled) {
        return false;
    }

    if (transaction_id != 0) {
        int duplicate = rfic_find_transaction(&state.write_queue,
                                              transaction_id, ch, cmd, data);
        if (duplicate != 0) {
            return duplicate > 0;
        }

        /* Tagged protocol is stop-and-wait. A second tagged command must not
         * sit behind a callback and overwrite its unacknowledged terminal
         * result before the host has observed it. A legacy untagged result
         * has no token to acknowledge, so the first tagged command may retire
         * it only after that new command is actually accepted. */
        if (!bladerf_rfic_tagged_write_allowed(
                state.write_queue.count, state.write_queue.last_rv_valid,
                state.write_queue.last_rv_transaction_id)) {
            return false;
        }
        clear_legacy_result = state.write_queue.last_rv_valid;
    }

    if (NULL != f && (NULL != f->write64 || NULL != f->write32) &&
        _check_validity(f, ch)) {
        rv = rfic_enqueue(&state.write_queue, ch, cmd, data,
                          transaction_id) != COMMAND_QUEUE_FULL;
        if (rv) {
            if (transaction_id != 0 && clear_legacy_result) {
                state.write_queue.last_rv_valid = false;
                state.write_queue.last_result_code =
                    BLADERF_RFIC_RESULT_NO_RESULT;
                state.write_queue.last_cleanup_result_code =
                    BLADERF_RFIC_RESULT_NO_RESULT;
            }
            if (transaction_id != 0) {
                state.write_queue.tagged_protocol_enabled = true;
            }
        }
    }

    RFIC_DBG("WR ", _rfic_cmdstr(cmd), _rfic_chanstr(ch), rv ? "ENQ" : "BAD",
             addr, data);

    return rv;
}

bool rfic_command_write_immed(bladerf_rfic_command cmd,
                              bladerf_channel ch,
                              uint64_t data)
{
    struct rfic_command_fns const *f = _get_cmd_ptr(cmd);

    bool rv = false;

    if (!rfic_calibration_command_allowed(
            state.calibration_faulted,
            cmd == BLADERF_RFIC_COMMAND_INIT)) {
        rv = false;
    } else if (NULL == f || !_check_validity(f, ch)) {
        rv = false;
    } else if (NULL != f->write64) {
        rv = f->write64(&state, ch, data);
    } else if (NULL != f->write32 && _rfic_write32_payload_valid(data)) {
        rv = f->write32(&state, ch, (uint32_t)data);
    }

    if (rfic_latch_calibration_fault()) {
        rv = false;
    }

    RFIC_DBG("WR!", _rfic_cmdstr(cmd), _rfic_chanstr(ch), rv ? "OK " : "BAD",
             0x0, data);

    return rv;
}

bool rfic_command_read(uint16_t addr, uint64_t *data)
{
    if (!_rfic_address_fields_valid(addr)) {
        if (data != NULL) {
            *data = UINT64_C(0xDEADBEEF0B4DF00D);
        }
        return false;
    }

    return rfic_command_read_immed(_rfic_unpack_cmd(addr),
                                   _rfic_unpack_chan(addr), data);
}

bool rfic_command_read_immed(bladerf_rfic_command cmd,
                             bladerf_channel ch,
                             uint64_t *data)
{
    struct rfic_command_fns const *f = _get_cmd_ptr(cmd);
    bool rv                          = false;

    if (data == NULL) {
        return false;
    }

    // Canary
    *data = 0xDEADBEEF0B4DF00D;

    if (state.calibration_faulted &&
        cmd != BLADERF_RFIC_COMMAND_STATUS &&
        cmd != BLADERF_RFIC_COMMAND_TRANSACTION_STATUS &&
        cmd != BLADERF_RFIC_COMMAND_PROGRESS &&
        cmd != BLADERF_RFIC_COMMAND_RESULT_DETAIL &&
        cmd != BLADERF_RFIC_COMMAND_INIT) {
        rv = false;
    } else if (NULL == f || !_check_validity(f, ch)) {
        rv = false;
    } else if (NULL != f->read64) {
        rv = f->read64(&state, ch, data);
    } else if (NULL != f->read32) {
        uint32_t data32;

        rv = f->read32(&state, ch, &data32);

        if (rv) {
            *data = (uint64_t)data32;
        }
    }

    RFIC_DBG("RD!", _rfic_cmdstr(cmd), _rfic_chanstr(ch), rv ? "OK " : "BAD",
             0x0, *data);

    return rv;
}

/**
 * @brief      Startup initialization for RFIC command
 */
void rfic_command_init(void)
{
    size_t i;
    /* The command state contains retained job flags and contexts in addition
     * to the queue. Reset the whole owner state on FPGA/NIOS startup so stale
     * RAM contents can never claim AD9361 ownership before the first command. */
    memset(&state, 0, sizeof(state));
    state.init_state = BLADERF_RFIC_INIT_STATE_OFF;
    state.frequency_invalid[BLADERF_MODULE_RX] = true;
    state.frequency_invalid[BLADERF_MODULE_TX] = true;
    for (i = 0; i < NUM_MODULES; i++) {
        state.samplerate_invalid[i] = true;
        state.bandwidth_invalid[i] = true;
        state.filter_invalid[i] = true;
    }
    rfic_queue_reset(&state.write_queue);
}

/**
 * @brief      Worker wrapper for RFIC command
 */
void rfic_command_work(void)
{
    struct rfic_queue_entry *active = rfic_queue_peek(&state.write_queue);
    const bool begins_init = active != NULL &&
        active->state == ENTRY_STATE_NEW &&
        active->cmd == BLADERF_RFIC_COMMAND_INIT &&
        active->value == BLADERF_RFIC_INIT_STATE_ON &&
        state.init_state != BLADERF_RFIC_INIT_STATE_ON;
    const bool measures_init = state.init_timing.active || begins_init;
    const uint64_t call_start = measures_init
        ? (uint64_t)alt_timestamp() : 0;

    if (state.init_timing.active && state.init_timing.have_worker_tick) {
        const uint64_t gap = call_start - state.init_timing.last_worker_ticks;
        if (gap > state.init_timing.max_service_gap_ticks)
            state.init_timing.max_service_gap_ticks = gap;
        state.init_timing.last_worker_ticks = call_start;
    }

    rfic_command_work_wq(&state.write_queue);

    if (measures_init) {
        const uint64_t elapsed = (uint64_t)alt_timestamp() - call_start;
        if (elapsed > state.init_timing.max_worker_ticks)
            state.init_timing.max_worker_ticks = elapsed;
    }
}

bool rfic_command_init_timing_read(uint8_t addr, uint64_t *value)
{
    if (value == NULL) return false;

    switch (addr) {
        case NIOS_PKT_8x64_RFIC_TIMING_STATUS:
            *value = ((uint64_t)state.init_timing.retirement_sequence << 32) |
                     ((uint64_t)state.init_timing.transaction_id << 16) |
                     ((uint64_t)state.init_timing.active << 1) |
                     (uint64_t)state.init_timing.valid;
            return true;
        case NIOS_PKT_8x64_RFIC_TIMING_TOTAL_TICKS:
        case NIOS_PKT_8x64_RFIC_TIMING_ACTIVE_ELAPSED_TICKS:
            *value = state.init_timing.active
                ? (uint64_t)alt_timestamp() - state.init_timing.start_ticks
                : state.init_timing.total_ticks;
            return state.init_timing.active || state.init_timing.valid ||
                   addr == NIOS_PKT_8x64_RFIC_TIMING_ACTIVE_ELAPSED_TICKS;
        case NIOS_PKT_8x64_RFIC_TIMING_MAX_WORKER_TICKS:
            *value = state.init_timing.max_worker_ticks;
            return state.init_timing.valid || state.init_timing.active;
        case NIOS_PKT_8x64_RFIC_TIMING_MAX_SERVICE_GAP_TICKS:
            *value = state.init_timing.max_service_gap_ticks;
            return state.init_timing.valid || state.init_timing.active;
        case NIOS_PKT_8x64_RFIC_TIMING_COUNTER_HZ:
            *value = alt_timestamp_freq();
            return *value != 0;
        case NIOS_PKT_8x64_RFIC_TIMING_INIT_STAGE:
            *value = state.init_stage;
            return true;
        case NIOS_PKT_8x64_RFIC_TIMING_RESULT_DETAIL:
            *value = ((uint64_t)state.init_timing.init_stage << 32) |
                     ((uint64_t)state.init_timing.tail_phase << 24) |
                     ((uint64_t)state.init_timing.module_index << 16) |
                     ((uint64_t)state.init_timing.cleanup_result_code << 8) |
                     state.init_timing.result_code;
            return true;
        case NIOS_PKT_8x64_RFIC_TIMING_ADI_STATUS:
            *value = (uint32_t)state.init_timing.adi_status;
            return true;
        case NIOS_PKT_8x64_RFIC_TIMING_ADI_CLEANUP_STATUS:
            *value = (uint32_t)state.init_timing.adi_cleanup_status;
            return true;
        case NIOS_PKT_8x64_RFIC_TIMING_ADI_STAGE:
            *value = state.init_timing.adi_stage;
            return true;
        case NIOS_PKT_8x64_RFIC_TIMING_SETUP_STAGE:
            *value = state.init_timing.setup_stage;
            return true;
        case NIOS_PKT_8x64_RFIC_TIMING_ERROR_STATUS:
            *value = (uint32_t)state.init_timing.primary_error_status;
            return true;
        case NIOS_PKT_8x64_RFIC_TIMING_ERROR_CONTEXT:
            *value = ((uint64_t)state.init_timing.error_stage << 32) |
                     ((uint64_t)state.init_timing.error_tail_phase << 24) |
                     ((uint64_t)state.init_timing.error_module_index << 16) |
                     state.init_timing.error_pending_mask;
            return true;
        default:
            *value = 0;
            return false;
    }
}

bool rfic_command_worker_active(void)
{
    return rfic_queue_peek(&state.write_queue) != NULL ||
           rfic_scheduler_calibration_pending(
               state.rfdc_job_pending, state.tx_quad_job_pending,
               state.sample_rate_job_pending, state.bandwidth_job_pending,
               state.init_job_pending || state.init_tail_pending ||
               state.rfpll_job_pending || state.fir_enable_job_pending);
}

uint8_t rfic_command_pending_mask(void)
{
    return (state.rfdc_job_pending ? 0x01u : 0u) |
           (state.tx_quad_job_pending ? 0x02u : 0u) |
           (state.sample_rate_job_pending ? 0x04u : 0u) |
           (state.bandwidth_job_pending ? 0x08u : 0u) |
           (state.init_job_pending ? 0x10u : 0u) |
           (state.init_tail_pending ? 0x20u : 0u) |
           (state.rfpll_job_pending ? 0x40u : 0u) |
           (state.fir_enable_job_pending ? 0x80u : 0u);
}


bool rfic_command_has_orphaned_jobs(void)
{
    return state.phy == NULL &&
           rfic_queue_peek(&state.write_queue) == NULL &&
           rfic_command_pending_mask() != 0;
}



bool rfic_command_owns_device(void)
{
    struct rfic_queue_entry *active = rfic_queue_peek(&state.write_queue);

    return rfic_scheduler_device_owned(
        active != NULL && active->state == ENTRY_STATE_RUNNING,
        rfic_scheduler_calibration_pending(
            state.rfdc_job_pending, state.tx_quad_job_pending,
            state.sample_rate_job_pending, state.bandwidth_job_pending,
            state.init_job_pending || state.init_tail_pending ||
                state.rfpll_job_pending || state.fir_enable_job_pending),
        adi_fastlock_recall_active() || adi_fastlock_load_active());
}

void rfic_command_progress_tick(void)
{
    rfic_queue_progress_tick(&state.write_queue);
}

#endif  // defined(BOARD_BLADERF_MICRO) && defined(BLADERF_NIOS_LIBAD936X)
