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

#if defined(BOARD_BLADERF_MICRO) && defined(BLADERF_NIOS_LIBAD936X)

#include <stdbool.h>
#include <stdint.h>
#include <errno.h>

#include "ad936x_helpers.h"
#include "devices_inline.h"
#include "devices_rfic.h"
#include "no_os_delay.h"
#include "rfic_cache_validity.h"
#include "rfic_calibration_policy.h"
#include "rfic_cal_done_policy.h"
#include "rfic_frequency_policy.h"
#include "rfic_init_policy.h"
#include "rfic_tx_output_policy.h"

/* The control PIO is clocked by sys_clock. This bit is driven by a separate
 * FPGA watchdog observing adi_rx_clock, so the preflight is independent of
 * the AD9361 register path. It verifies that the RF data interface clock is
 * present before initializing the datapath; it is not an Avalon timeout. */
#define BLADERF_AD9361_IF_CLOCK_WAIT_ATTEMPTS 200
#define BLADERF_AD9361_IF_CLOCK_POLL_US       100

static bool _wait_for_ad9361_if_clock(void)
{
    unsigned int attempt;

    for (attempt = 0; attempt < BLADERF_AD9361_IF_CLOCK_WAIT_ATTEMPTS;
         ++attempt) {
        if ((control_reg_read() & CONTROL_AD9361_RX_IF_CLOCK_ALIVE) != 0) {
            return true;
        }
        no_os_udelay(BLADERF_AD9361_IF_CLOCK_POLL_US);
    }

    return false;
}

/* AD9361 CTRL_OUT[7] carries Cal Done for CTRL_OUT pointer 0 (UG-570 Table
 * 44). The xA4 RFFE GPIO input bus routes CTRL_OUT[7] to bit 31 and clocks it
 * through a synchronizer into sys_clock. */
static int32_t _rfic_read_cal_done(struct ad9361_rf_phy *phy, uint32_t mask,
				   bool *done)
{
    uint32_t gpio;
    uint8_t output_bit;

    if (phy == NULL || phy->pdata == NULL || done == NULL ||
        phy->pdata->ctrl_outs_ctrl.index != 0 ||
        !rfic_cal_done_output_bit(mask, &output_bit) ||
        (phy->pdata->ctrl_outs_ctrl.en_mask & (1u << output_bit)) == 0) {
        return -ENOTSUP;
    }
    if (!rffe_csr_read_checked(&gpio)) {
        return -EIO;
    }

    *done = (gpio & (1u << (RFFE_CONTROL_CTRL_OUT + output_bit))) != 0;
    return 0;
}

/* An LO jump strictly greater than this size invalidates the RF DC offset
 * calibration for the previous frequency (ADI EngineerZone, "AD9361
 * RF DC offset calibration timeout": "if the frequency change is more
 * than 100MHz, you need to run DC offset and QEC init cals in order
 * to get optimum performance"). ad9361_setup() only runs RFDC_CAL
 * once, at init, on whatever LO the init params specify -- nothing
 * in this driver reran it on retune before this fix, so every RX
 * tune more than 100MHz from that one calibrated point carried a
 * stale DC-offset correction indefinitely. */
#define BLADERF_AD9361_RFDC_CAL_RETUNE_THRESHOLD_HZ 100000000ULL

/* The NIOS-owned PHY deliberately has no axi_adc handle. Production uses
 * digital_interface_tune_skip_mode=2 and preset DATA_CLK delays, so the NIOS
 * sample-rate path must not invoke the BIST tuner. The host-owned PHY and its
 * converter handle are separate from this NIOS instance. */

extern AD9361_InitParam bladerf2_rfic_init_params;
extern AD9361_RXFIRConfig bladerf2_rfic_rx_fir_config;
extern AD9361_TXFIRConfig bladerf2_rfic_tx_fir_config;
extern AD9361_RXFIRConfig bladerf2_rfic_rx_fir_config_dec2;
extern AD9361_TXFIRConfig bladerf2_rfic_tx_fir_config_int2;
extern AD9361_RXFIRConfig bladerf2_rfic_rx_fir_config_dec4;
extern AD9361_TXFIRConfig bladerf2_rfic_tx_fir_config_int4;

static bool _rfic_cmd_wr_frequency_internal(struct rfic_state *state,
                                            bladerf_channel channel,
                                            uint64_t frequency,
                                            bool init_sequence);
static bool _rfic_cmd_wr_filter_init_pair(struct rfic_state *state,
                                          bladerf_channel channel,
                                          uint32_t filter);

static bool _rfic_ensure_rx_rfdc_calibrated(
    struct rfic_state *state, bladerf_frequency frequency)
{
    uint32_t rx_port;

    CHECK_BOOL(ad9361_get_rx_rf_port_input(state->phy, &rx_port));
    if (rx_port >= RFIC_RX_RF_PORT_COUNT) {
        RFIC_ERR("invalid RX RF port for RFDC history", -EINVAL);
        return false;
    }

    if (!rfic_rfdc_port_calibration_required(
            state->last_rfdc_calib_freq[BLADERF_RX], rx_port, frequency,
            BLADERF_AD9361_RFDC_CAL_RETUNE_THRESHOLD_HZ)) {
        return true;
    }

    /* This NIOS command path must never wait for a complete RFDC calibration
     * in one worker turn. The retained job owns completion and cache commit. */
    int32_t status = ad9361_rfdc_calibration_start(state->phy,
                                                   &state->job_context.rfdc_job);
    if (status < 0) {
        RFIC_ERR("ad9361_rfdc_calibration_start", status);
        return false;
    }
    state->rfdc_job_pending = true;
    state->rfdc_job_port = (uint8_t)rx_port;
    state->rfdc_job_frequency = frequency;
    return true;
}


/******************************************************************************/
/* Helpers */
/******************************************************************************/

/**
 * @brief      Clear RFFE control bits
 */
static bool _clear_rffe_ctrl(void)
{
    uint32_t reg;

    if (!rffe_csr_read_checked(&reg)) {
        return false;
    }
    reg &= ~(1 << RFFE_CONTROL_TXNRX);
    reg &= ~(1 << RFFE_CONTROL_ENABLE);
    reg &= ~(RFFE_CONTROL_SPDT_MASK << RFFE_CONTROL_RX_SPDT_1);
    reg &= ~(RFFE_CONTROL_SPDT_MASK << RFFE_CONTROL_RX_SPDT_2);
    reg &= ~(RFFE_CONTROL_SPDT_MASK << RFFE_CONTROL_TX_SPDT_1);
    reg &= ~(RFFE_CONTROL_SPDT_MASK << RFFE_CONTROL_TX_SPDT_2);
    reg &= ~(1 << RFFE_CONTROL_MIMO_RX_EN_0);
    reg &= ~(1 << RFFE_CONTROL_MIMO_TX_EN_0);
    reg &= ~(1 << RFFE_CONTROL_MIMO_RX_EN_1);
    reg &= ~(1 << RFFE_CONTROL_MIMO_TX_EN_1);
    return rffe_csr_write_checked(
        reg, rfic_disconnect_verify_mask(
                 (1u << RFFE_CONTROL_ADF_MUXOUT) - 1u,
                 1u << RFFE_CONTROL_RESET_N));
}

/* A failed RF-port/ENSM/RFFE sequence can leave the board path different from
 * the last committed host state. Do not allow another RF mutation to build on
 * that partial state. INIT/ON will deinitialize, pulse RESETB, and establish
 * a new complete state. The second disconnect attempt is best-effort; the
 * fault latch remains set even if the RFFE readback still disagrees. */
static void _rfic_fail_closed_after_path_mutation(struct rfic_state *state,
                                                  const char *reason)
{
    state->calibration_faulted = true;
    state->init_state = BLADERF_RFIC_INIT_STATE_OFF;
    state->frequency_invalid[BLADERF_RX] = true;
    state->frequency_invalid[BLADERF_TX] = true;
    rfic_cache_invalidate_all(state->samplerate_invalid, NUM_MODULES);
    rfic_cache_invalidate_all(state->bandwidth_invalid, NUM_MODULES);
    rfic_cache_invalidate_all(state->filter_invalid, NUM_MODULES);

    RFIC_ERR(reason, -EIO);
    if (!rfic_force_rf_path_off()) {
        rfic_command_record_cleanup_error(-EIO);
        DBG("%s: RFFE disconnect readback failed; reset remains required\n",
            __FUNCTION__);
    }
}

bool rfic_force_rf_path_off(void)
{
    return _clear_rffe_ctrl();
}

/* Disconnect only the TX side so a failed post-retune calibration cannot
 * leave an unqualified RF path active. RX state is preserved. */
bool rfic_disable_tx_output(void)
{
    uint32_t reg;

    if (!rffe_csr_read_checked(&reg)) {
        return false;
    }
    reg = rfic_tx_disconnect_value(
        reg, 1u << RFFE_CONTROL_TXNRX,
        (1u << RFFE_CONTROL_MIMO_TX_EN_0) |
            (1u << RFFE_CONTROL_MIMO_TX_EN_1),
        (RFFE_CONTROL_SPDT_MASK << RFFE_CONTROL_TX_SPDT_1) |
            (RFFE_CONTROL_SPDT_MASK << RFFE_CONTROL_TX_SPDT_2));
    return rffe_csr_write_checked(reg, RFFE_CSR_OUTPUT_MASK);
}

/**
 * @brief      Controls the RFIC hard reset bit
 *
 * @param[in]  state  true to reset the device, false to un-reset device
 */
static bool _reset_rfic(bool state)
{
    uint32_t reg;

    if (!rffe_csr_read_checked(&reg)) {
        return false;
    }

    if (state) {
        reg &= ~(1 << RFFE_CONTROL_RESET_N);
    } else {
        reg |= 1 << RFFE_CONTROL_RESET_N;
    }

    return rffe_csr_write_checked(reg, 1u << RFFE_CONTROL_RESET_N);
}


/******************************************************************************/
/* Initialization state changers */
/******************************************************************************/

/**
 * @brief      Deinitializes RFIC and RFFE
 *
 * @return     true if successful, false if not
 */
static bool _rfic_deinitialize(struct rfic_state *state,
                               bool board_reset_asserted,
                               bool report_cleanup_errors)
{
    state->frequency_invalid[BLADERF_RX] = true;
    state->frequency_invalid[BLADERF_TX] = true;
    rfic_cache_invalidate_all(state->samplerate_invalid, NUM_MODULES);
    rfic_cache_invalidate_all(state->bandwidth_invalid, NUM_MODULES);
    rfic_cache_invalidate_all(state->filter_invalid, NUM_MODULES);

    /* Unset RFFE bits controlling RFIC */
    const bool rffe_cleared = _clear_rffe_ctrl();
    if (!rffe_cleared && report_cleanup_errors)
        rfic_command_record_cleanup_error(-EIO);
    if (board_reset_asserted && !_reset_rfic(true)) {
        RFIC_ERR("AD9361 RESETB lost before PHY teardown", -EIO);
        return false;
    }
    /* RFFE is already unavailable. If ADI teardown fails, do not keep
     * advertising the previous ON/STANDBY state as usable. */
    state->init_state = BLADERF_RFIC_INIT_STATE_OFF;
    rfic_rfdc_calibration_history_reset(
        state->last_rfdc_calib_freq[BLADERF_RX]);
    rfic_rfdc_calibration_history_reset(
        state->last_rfdc_calib_freq[BLADERF_TX]);

    /* Deinitialize AD9361
     *
     * ad9361_deinit() was a local addition to the driver revision this used
     * to build against; upstream provides ad9361_remove() instead, which
     * frees more (clocks, SPI and all GPIO descriptors on top of the data).
     * Mirrors the host side fix in rfic_host.c. */
    if (NULL != state->phy) {
        struct ad9361_rf_phy *phy = state->phy;

        {
            bool consumed = false;
            int32_t remove_status = ad9361_remove_ex(
                phy, board_reset_asserted, &consumed);

            if (consumed)
                state->phy = NULL;
            if (remove_status < 0) {
                state->calibration_faulted = true;
                if (report_cleanup_errors)
                    rfic_command_record_cleanup_error(remove_status);
                RFIC_ERR("ad9361_remove", remove_status);
                if (!rffe_cleared) {
                    _rfic_fail_closed_after_path_mutation(
                        state, "RFFE disconnect also failed during deinit");
                }
                return false;
            }
        }
    }

    if (!rffe_cleared) {
        _rfic_fail_closed_after_path_mutation(
            state, "RFFE did not confirm RF disconnect during deinit");
    }

    /* A terminal constructor context may be reused only after its candidate
     * PHY has been consumed. Preserve it if removal failed and ownership is
     * still with this state object. */
    if (state->phy == NULL) {
        state->init_job_pending = false;
        state->init_job_started = false;
        state->init_job_status = 0;
        state->init_tail_phase = RFIC_INIT_TAIL_IDLE;
        state->init_tail_pending = false;
        state->init_module_index = 0;
        state->init_channel_index = 0;
        state->init_restore_standby = false;
        state->init_nested_complete = false;
        state->init_nested_status = 0;
        state->rfpll_job_pending = false;
        state->frequency_rfpll_resume_command = false;
        state->frequency_rfpll_status = 0;
        state->fir_enable_job_pending = false;
        state->fir_enable_resume_command = false;
        state->fir_enable_init_pair_resume = false;
    }

    /* The chip is deliberately left out of hardware reset, matching
     * _rfic_host_deinitialize. Holding it in reset stops if_l_clk, and the
     * FPGA's AXI ad9361 core runs on that clock: the next Avalon access to it
     * would stall the Nios with no way back short of reloading the
     * bitstream. _clear_rffe_ctrl() above already dropped the control bits,
     * which is what "deinitialized" has to mean here. */

    DBG("*** RFIC Control Deinitialized ***\n");

    return rffe_cleared;
}

static void _rfic_init_tail_advance_module(struct rfic_state *state,
                                          bool restore_standby_state)
{
    state->init_module_index++;
    if (state->init_module_index >= NUM_MODULES) {
        state->init_channel_index = 0;
        state->init_tail_phase = restore_standby_state
            ? RFIC_INIT_TAIL_TX_MUTE : RFIC_INIT_TAIL_TX_GAIN;
    } else {
        state->init_tail_phase = RFIC_INIT_TAIL_FILTER;
    }
}

/* The wire format has four step bits. Report the actual retained ADI setup
 * phase as a stable coarse stage instead of leaving INIT stuck at the generic
 * SETUP_POLL value for the entire constructor. */
static uint8_t _rfic_init_progress_step(const struct rfic_state *state)
{
    uint8_t const stage = (uint8_t)state->init_job.stage;

    if (state->init_job_started &&
        (state->init_job_pending || state->phy == NULL ||
         state->init_tail_phase == RFIC_INIT_TAIL_IDLE)) {
        switch (stage) {
        case AD9361_INIT_TXN_RESET_ASSERT:
        case AD9361_INIT_TXN_RESET_LOW_WAIT:
        case AD9361_INIT_TXN_RESET_RELEASE:
        case AD9361_INIT_TXN_RESET_HIGH_WAIT:
            return BLADERF_RFIC_PROGRESS_INIT_RESET;
        case AD9361_INIT_TXN_PRODUCT_ID:
        case AD9361_INIT_TXN_LOCK_READ:
        case AD9361_INIT_TXN_LOCK_WRITE:
        case AD9361_INIT_TXN_LOCK_VERIFY:
            return BLADERF_RFIC_PROGRESS_INIT_IDENTIFY_LOCK;
        case AD9361_INIT_TXN_CLOCKS:
            return BLADERF_RFIC_PROGRESS_INIT_CLOCKS;
        case AD9361_INIT_TXN_SETUP_START:
            return BLADERF_RFIC_PROGRESS_INIT_PRE_CLOCK;
        case AD9361_INIT_TXN_SETUP_POLL: {
            uint8_t const setup = (uint8_t)state->init_job.setup_job.stage;

            if (setup < AD9361_SETUP_TRANSACTION_CLOCK_CHAIN_START)
                return BLADERF_RFIC_PROGRESS_INIT_PRE_CLOCK;
            if (setup <= AD9361_SETUP_TRANSACTION_CLOCK_CHAIN_WAIT)
                return BLADERF_RFIC_PROGRESS_INIT_CLOCK_CHAIN;
            if (setup < AD9361_SETUP_TRANSACTION_RX_CP_START)
                return BLADERF_RFIC_PROGRESS_INIT_POST_CLOCK;
            if (setup <= AD9361_SETUP_TRANSACTION_TX_CP_WAIT)
                return BLADERF_RFIC_PROGRESS_INIT_SYNTH_CP;
            if (setup < AD9361_SETUP_TRANSACTION_GM_START)
                return BLADERF_RFIC_PROGRESS_INIT_POST_CP;
            if (setup <= AD9361_SETUP_TRANSACTION_GM_STOP)
                return BLADERF_RFIC_PROGRESS_INIT_GAIN_TABLE;
            if (setup <= AD9361_SETUP_TRANSACTION_GC_UPDATE)
                return BLADERF_RFIC_PROGRESS_INIT_GAIN_CONTROL;
            return BLADERF_RFIC_PROGRESS_INIT_CALIBRATION;
        }
        case AD9361_INIT_TXN_PLATFORM_POST_SETUP:
            return BLADERF_RFIC_PROGRESS_INIT_CALIBRATION;
        default:
            break;
        }
    }

    switch (state->init_tail_phase) {
    case RFIC_INIT_TAIL_RESET_LO:
        return BLADERF_RFIC_PROGRESS_INIT_TAIL_RESET_LO;
    case RFIC_INIT_TAIL_FILTER:
        return BLADERF_RFIC_PROGRESS_INIT_TAIL_FILTER;
    case RFIC_INIT_TAIL_SAMPLE_RATE:
        return BLADERF_RFIC_PROGRESS_INIT_TAIL_SAMPLE_RATE;
    case RFIC_INIT_TAIL_FINAL_LO:
    case RFIC_INIT_TAIL_TX_GAIN:
    case RFIC_INIT_TAIL_TX_MUTE:
    case RFIC_INIT_TAIL_DONE:
        return BLADERF_RFIC_PROGRESS_INIT_TAIL_FINAL_LO_OUTPUT;
    default:
        return BLADERF_RFIC_PROGRESS_INIT_UNKNOWN;
    }
}

/**
 * @brief      Initializes RFIC and RFFE
 *
 * @return     true if successful, false if not
 */
static bool _rfic_initialize(struct rfic_state *state)
{
    AD9361_InitParam *init_param = &bladerf2_rfic_init_params;
    bladerf_direction dir;
    bladerf_channel ch;
    bool const ad9361_init_resume = rfic_init_adi_resume(
        state->init_job_started, state->init_job_pending,
        state->init_job_status, state->phy != NULL);
    bool const restore_standby_state = state->init_tail_phase !=
                                               RFIC_INIT_TAIL_IDLE
        ? state->init_restore_standby
        : rfic_init_restore_standby_state(
              state->init_state == BLADERF_RFIC_INIT_STATE_STANDBY,
              state->phy != NULL);

    if (state->init_job_started && !state->init_job_pending &&
        state->init_job_status < 0) {
        state->calibration_faulted = true;
        rfic_command_record_cleanup_error(state->init_job.cleanup_status);
        state->init_stage = rfic_init_cleanup_stage(
            state->init_job.cleanup_status, state->init_stage,
            BLADERF_RFIC_INIT_STAGE_CLEANUP_FAILED);
        RFIC_ERR("AD9361 init transaction", state->init_job_status);
        return false;
    }

    /* Do not replay the RFFE disconnect, cache invalidation, clock preflight,
     * or reset release after the ADI candidate has completed. In particular,
     * _clear_rffe_ctrl() would drop ENABLE after the setup cursor just used
     * the datapath. Resume at the common PHY validation/tail boundary. */
    if (ad9361_init_resume)
        goto ad9361_init_ready;

    state->init_stage = BLADERF_RFIC_INIT_STAGE_ENTER;
    /* Initialization can mutate clocks, ports, FIR and LO in stages. Until
     * every stage completes, neither ON nor STANDBY is a valid status. */
    state->init_state = BLADERF_RFIC_INIT_STATE_OFF;
    if (state->phy == NULL &&
        !rfic_init_digital_tune_policy_valid(
            init_param->digital_interface_tune_skip_mode,
            init_param->digital_interface_tune_fir_disable != 0)) {
        state->init_stage = BLADERF_RFIC_INIT_STAGE_DIG_TUNE_POLICY;
        RFIC_ERR("NIOS requires preset digital-interface delays",
                 -EINVAL);
        return false;
    }
    /* A new PHY has its own RFDC calibration baseline. Never reuse the
     * previous instance's frequency when deciding whether a retune needs
     * calibration. */
    if (!restore_standby_state) {
        /* A new PHY starts with no trusted LO readback. The per-module init
         * sequence establishes both directions before normal retunes resume. */
        state->frequency_invalid[BLADERF_RX] = true;
        state->frequency_invalid[BLADERF_TX] = true;
        rfic_cache_invalidate_all(state->samplerate_invalid, NUM_MODULES);
        rfic_cache_invalidate_all(state->bandwidth_invalid, NUM_MODULES);
        rfic_cache_invalidate_all(state->filter_invalid, NUM_MODULES);
        rfic_rfdc_calibration_history_reset(
            state->last_rfdc_calib_freq[BLADERF_RX]);
        rfic_rfdc_calibration_history_reset(
            state->last_rfdc_calib_freq[BLADERF_TX]);
    }

    /* Unset RFFE bits controlling RFIC */
    if (!_clear_rffe_ctrl()) {
        _rfic_fail_closed_after_path_mutation(
            state, "RFFE did not confirm RF disconnect before init");
        return false;
    }
    state->init_stage = BLADERF_RFIC_INIT_STAGE_RFFE_CLEARED;

    /* Initialize AD9361 if it isn't initialized */
    if (NULL == state->phy) {
        state->init_state = BLADERF_RFIC_INIT_STATE_OFF;

        if (state->init_job_started) {
            /* The queue worker has completed the retained ADI constructor.
             * Its exact status and candidate ownership survive until this
             * callback continuation handles success or cleanup. */
            if (state->init_job_pending) {
                RFIC_ERR("INIT callback resumed while ADI job is pending",
                         -EBUSY);
                return false;
            }
            if (state->init_job_status < 0) {
                state->calibration_faulted = true;
                RFIC_ERR("ad9361_init_transaction_poll",
                         state->init_job_status);
                return false;
            }
            if (state->init_job_status != 0 || state->phy == NULL) {
                state->init_stage = BLADERF_RFIC_INIT_STAGE_PHY_NULL;
                RFIC_ERR("ADI init transaction completed without PHY", -EIO);
                return false;
            }
        } else {

        DBG("--- Initializing AD9361 ---\n");

        /* No hard reset here, deliberately. The host path does not do one
         * either (_rfic_host_initialize clears the RFFE control bits and goes
         * straight into ad9361_init). Keep the RFIC out of hardware reset
         * while initializing its FPGA datapath. The AXI-Lite adapter has a
         * short hardware response timeout, so a missing internal response
         * returns its 0xDEADDEAD read sentinel rather than holding the NIOS
         * Avalon master forever. How that sentinel and write timeouts affect
         * this initialization path still needs explicit error propagation.
         *
         * Release the pin instead: after a cold power-up the part may still
         * be held in reset, and it has to be running before ad9361_init
         * touches the AXI core. */
        CHECK_TRUE(_reset_rfic(false));
        no_os_mdelay(1);
        state->init_stage = BLADERF_RFIC_INIT_STAGE_RESET_OUT;

        /* ad9361_init() initializes the FPGA's AD9361 datapath. Keep the
         * AD9361 interface clock running while that setup occurs. The
         * independent clock preflight below is a fail-fast readiness check;
         * the ADI AXI-Lite adapter itself has a finite response timeout and
         * returns 0xDEADDEAD on a timed-out read. A NIOS no-response event is
         * therefore not yet explained by an unbounded Avalon waitrequest.
         *
         * Earlier AXI_ADC_NOT_PRESENT ablation changed the observed failure
         * into a returned initialization error, but did not isolate whether
         * the cause was the clock, the timeout sentinel, or a later driver
         * wait. Preserve this as an unresolved diagnostic, not a root cause.
         *
         * Nothing else in this firmware ever sets ENABLE (the host does it
         * from enable_module), so raise it here and give the interface time
         * to come up before the driver touches the core. */
        {
            uint32_t reg;

            if (!rffe_csr_read_checked(&reg)) {
                RFIC_ERR("RFFE read before AD9361 init", -EIO);
                return false;
            }

            reg |= (1 << RFFE_CONTROL_ENABLE);
            CHECK_TRUE(rffe_csr_write_checked(
                reg, 1u << RFFE_CONTROL_ENABLE));
        }
        state->init_stage = BLADERF_RFIC_INIT_STAGE_ENABLE_SET;

        /* Require the independently monitored AD9361 interface clock before
         * entering no-OS datapath initialization. This avoids continuing with
         * an unavailable sample interface; AXI response timeout/error
         * handling is a separate concern. */
        state->init_stage = BLADERF_RFIC_INIT_STAGE_IF_CLOCK_WAIT;
        if (!_wait_for_ad9361_if_clock()) {
            state->init_stage = BLADERF_RFIC_INIT_STAGE_IF_CLOCK_TIMEOUT;
            RFIC_ERR("AD9361 RX interface clock preflight", -ETIMEDOUT);
            DBG("%s: AD9361 RX interface clock did not start\n",
                __FUNCTION__);
            return false;
        }
        state->init_stage = BLADERF_RFIC_INIT_STAGE_IF_CLOCK_OK;

        /* ad9361_init() used to take the device handle as a third argument
         * and stash it for the SPI and GPIO accessors. On this platform
         * there is no such handle: the Nios SPI/GPIO adapters
         * (bladerf2_headless_spi.c/bladerf2_headless_gpio.c) reach the RFIC
         * through registers directly, not through desc->extra. Mirrors the
         * host side fix in rfic_host.c, minus the .extra wiring host needs
         * for its USB backend. */
        state->init_stage = BLADERF_RFIC_INIT_STAGE_AD9361_INIT;
        {
            int32_t init_status = ad9361_init_transaction_start(
                &state->init_job, &state->phy, init_param);

            if (init_status < 0) {
                RFIC_ERR("ad9361_init_transaction_start", init_status);
                return false;
            }

            state->init_job_started = true;
            state->init_job_pending = true;
            state->init_job_status = 0;
        }
        /* Keep the same queue item RUNNING. The worker polls one retained
         * ADI phase on each later pass and re-enters this callback only at
         * the constructor's terminal result. */
        return true;
        }
        state->init_stage = BLADERF_RFIC_INIT_STAGE_PHY_OK;

        if (NULL == state->phy) {
            state->init_stage = BLADERF_RFIC_INIT_STAGE_PHY_NULL;
            RFIC_ERR("ad9361_init returned NULL PHY", -EIO);
            DBG("%s: ad9361_init returned ok but left phy NULL\n",
                __FUNCTION__);
            return false;
        }

    }

ad9361_init_ready:
    state->init_stage = BLADERF_RFIC_INIT_STAGE_PHY_OK;

    if (NULL == state->phy) {
        state->init_stage = BLADERF_RFIC_INIT_STAGE_PHY_NULL;
        RFIC_ERR("ad9361_init returned NULL PHY", -EIO);
        return false;
    }

    if (NULL == state->phy->pdata) {
        state->init_stage = BLADERF_RFIC_INIT_STAGE_PDATA_NULL;
        RFIC_ERR("ad9361_init returned PHY without platform data", -EIO);
        DBG("%s: ad9361_init failed silently\n", __FUNCTION__);
        struct ad9361_rf_phy *phy = state->phy;
        bool consumed = false;
        int remove_status = ad9361_remove_ex(phy, false, &consumed);
        if (consumed)
            state->phy = NULL;
        state->init_stage = rfic_init_cleanup_stage(
            remove_status, state->init_stage,
            BLADERF_RFIC_INIT_STAGE_CLEANUP_FAILED);
        if (remove_status < 0) {
            rfic_command_record_cleanup_error(remove_status);
            DBG("%s: cleanup of invalid AD9361 context failed (%d)\n",
                __FUNCTION__, remove_status);
        }
        return false;
    }

    /* Use the board-routed AD9361 completion signal after constructor
     * completion, then seed the RFDC history from setup's verified RX LO. */
    state->phy->read_cal_done = _rfic_read_cal_done;
    if (init_param->rx_rf_port_input_select < RFIC_RX_RF_PORT_COUNT) {
        state->last_rfdc_calib_freq[BLADERF_RX]
                                   [init_param->rx_rf_port_input_select] =
            init_param->rx_synthesizer_frequency_hz;
    }

    if (state->init_tail_phase == RFIC_INIT_TAIL_IDLE) {
        state->init_restore_standby = restore_standby_state;
        state->init_module_index = 0;
        state->init_channel_index = 0;
        state->init_nested_complete = false;
        state->init_nested_status = 0;
        /* The retained ADI constructor has already configured each operating
         * LO and completed setup RFDC on the configured RX input. The old
         * 70 MHz RX staging hop left that input unchanged while RX was off;
         * INIT policy skipped RFDC at the staging point, then returned to the
         * already-calibrated LO. The actual band-dependent RX input is
         * selected and calibrated by the RX-enable path. Do not retune the
         * RFPLL away from and back to the constructor's verified LO here. */
        state->init_tail_phase = restore_standby_state
            ? RFIC_INIT_TAIL_TX_MUTE : RFIC_INIT_TAIL_FILTER;
    }

    state->init_tail_pending = false;
    if (state->init_nested_complete) {
        state->init_nested_complete = false;
        if (state->init_nested_status < 0)
            return false;

        /* The LO was already programmed before its retained calibration
         * started. Resume after that LO phase; never replay the setter. */
        if (state->init_tail_phase == RFIC_INIT_TAIL_FINAL_LO) {
            _rfic_init_tail_advance_module(state, restore_standby_state);
        } else {
            RFIC_ERR("unexpected INIT nested-job continuation", -EIO);
            return false;
        }
    }

    if (state->init_tail_phase <= RFIC_INIT_TAIL_FINAL_LO) {
        bladerf_frequency target_frequency;
        bladerf_sample_rate target_sample_rate;

        state->init_stage = BLADERF_RFIC_INIT_STAGE_PER_MODULE;
        dir = (bladerf_direction)state->init_module_index;
        ch = CHANNEL_IDX(dir, 0);
        target_frequency = dir == BLADERF_TX
            ? init_param->tx_synthesizer_frequency_hz
            : init_param->rx_synthesizer_frequency_hz;
        target_sample_rate = dir == BLADERF_TX
            ? init_param->tx_path_clock_frequencies[5]
            : init_param->rx_path_clock_frequencies[5];

        switch (state->init_tail_phase) {
        case RFIC_INIT_TAIL_FILTER:
            if (!_rfic_cmd_wr_filter_init_pair(
                    state, ch, dir == BLADERF_TX
                        ? BLADERF_RFIC_TXFIR_DEFAULT
                        : BLADERF_RFIC_RXFIR_DEFAULT))
                return false;
            if (state->fir_enable_job_pending) {
                state->init_tail_pending = true;
                return true;
            }
            if (state->init_module_index + 1 < NUM_MODULES) {
                _rfic_init_tail_advance_module(
                    state, restore_standby_state);
                state->init_tail_pending = true;
                return true;
            }
            state->init_tail_phase = RFIC_INIT_TAIL_SAMPLE_RATE;
            state->init_tail_pending = true;
            return true;

        case RFIC_INIT_TAIL_SAMPLE_RATE:
            if (!_rfic_cmd_wr_samplerate(state, ch, target_sample_rate))
                return false;
            if (state->sample_rate_job_pending) {
                state->init_tail_pending = true;
                return true;
            }
            state->init_tail_phase = RFIC_INIT_TAIL_FINAL_LO;
            state->init_tail_pending = true;
            return true;

        case RFIC_INIT_TAIL_FINAL_LO:
            state->init_nested_status = 0;
            if (!_rfic_cmd_wr_frequency_internal(
                    state, ch, target_frequency, true))
                return false;
            if (state->rfdc_job_pending || state->tx_quad_job_pending) {
                state->init_tail_pending = true;
                return true;
            }
            _rfic_init_tail_advance_module(state, restore_standby_state);
            state->init_tail_pending = true;
            return true;

        default:
            RFIC_ERR("invalid INIT module phase", -EIO);
            return false;
        }
    }

    state->init_stage = BLADERF_RFIC_INIT_STAGE_PER_CHANNEL;
    switch (state->init_tail_phase) {
    case RFIC_INIT_TAIL_TX_GAIN:
        ch = CHANNEL_IDX(BLADERF_TX, state->init_channel_index);
        if (!txmute_set_cached(state->phy, ch,
                               init_param->tx_attenuation_mdB))
            return false;
        state->init_tail_phase = RFIC_INIT_TAIL_TX_MUTE;
        state->init_tail_pending = true;
        return true;

    case RFIC_INIT_TAIL_TX_MUTE:
        ch = CHANNEL_IDX(BLADERF_TX, state->init_channel_index);
        if (restore_standby_state) {
            CHECK_BOOL(txmute_set(state->phy, ch,
                                  state->tx_mute_state[
                                      state->init_channel_index]));
        } else {
            CHECK_BOOL(txmute_set(state->phy, ch, false));
        }
        state->init_channel_index++;
        if (state->init_channel_index >= 2) {
            state->init_tail_phase = RFIC_INIT_TAIL_DONE;
        } else if (!restore_standby_state) {
            state->init_tail_phase = RFIC_INIT_TAIL_TX_GAIN;
        }
        state->init_tail_pending = true;
        return true;

    case RFIC_INIT_TAIL_DONE:
        state->init_tail_pending = false;
        break;

    default:
        RFIC_ERR("invalid INIT channel phase", -EIO);
        return false;
    }

    /* Update state */
    state->init_state = BLADERF_RFIC_INIT_STATE_ON;
    state->init_job_started = false;
    state->init_job_pending = false;
    state->init_job_status = 0;
    state->init_tail_phase = RFIC_INIT_TAIL_IDLE;
    state->init_tail_pending = false;
    state->init_nested_complete = false;
    if (!restore_standby_state) {
        rfic_cache_validate_all(state->samplerate_invalid, NUM_MODULES);
        rfic_cache_validate_all(state->bandwidth_invalid, NUM_MODULES);
        rfic_cache_validate_all(state->filter_invalid, NUM_MODULES);
    }
    state->init_stage = BLADERF_RFIC_INIT_STAGE_DONE;

    DBG("*** RFIC Control Initialized ***\n");

    return true;
}

/**
 * @brief      Puts RFIC and RFFE into standby ("warm de-initialization")
 *
 * @return     true if successful, false if not
 */
static bool _rfic_standby(struct rfic_state *state)
{
    AD9361_InitParam *init_param = &bladerf2_rfic_init_params;
    bladerf_channel ch;
    size_t i;

    /* If we aren't initialized, just go directly to deinitialization */
    if (NULL == state->phy) {
        return _rfic_deinitialize(state, false, false);
    }

    /* Unset RFFE bits controlling RFIC */
    /* A failed mute snapshot/write must not leave status reporting ON after
     * the RF enable path has already been cleared. */
    state->init_state = BLADERF_RFIC_INIT_STATE_OFF;
    if (!_clear_rffe_ctrl()) {
        _rfic_fail_closed_after_path_mutation(
            state, "RFFE did not confirm standby disconnect");
        return false;
    }

    /* Cache current txmute state and put it aside */
    FOR_EACH_CHANNEL(BLADERF_TX, 2, i, ch)
    {
        CHECK_BOOL(txmute_get(state->phy, ch, &state->tx_mute_state[i]));
        CHECK_BOOL(txmute_set(state->phy, ch, true));
    }

    /* Reset frequencies if they're invalid */
    if (state->frequency_invalid[BLADERF_TX]) {
            CHECK_TRUE(
            _rfic_cmd_wr_frequency(state, BLADERF_CHANNEL_TX(0),
                                   init_param->tx_synthesizer_frequency_hz));
    }

    if (state->frequency_invalid[BLADERF_RX]) {
            CHECK_TRUE(
            _rfic_cmd_wr_frequency(state, BLADERF_CHANNEL_RX(0),
                                   init_param->rx_synthesizer_frequency_hz));
    }

    /* Update state */
    state->init_state = BLADERF_RFIC_INIT_STATE_STANDBY;

    DBG("*** RFIC Control Standby ***\n");

    return true;
}


/******************************************************************************/
/* Command implementations */
/******************************************************************************/

/**
 * @brief       Read system status register
 *
 * @param[out]  status  Status register value
 *
 * @return      true if successful, false if not
 */
bool _rfic_cmd_rd_status(struct rfic_state *state,
                         bladerf_channel channel,
                         uint64_t *status)
{
    struct rfic_queue_entry *active =
        rfic_queue_peek(&state->write_queue);

    *status = ((_rfic_is_initialized(state) & BLADERF_RFIC_STATUS_INIT_MASK)
               << BLADERF_RFIC_STATUS_INIT_SHIFT) |

              ((state->write_queue.count & BLADERF_RFIC_STATUS_WQLEN_MASK)
               << BLADERF_RFIC_STATUS_WQLEN_SHIFT) |

              ((state->write_queue.last_rv & BLADERF_RFIC_STATUS_WQSUCCESS_MASK)
               << BLADERF_RFIC_STATUS_WQSUCCESS_SHIFT) |

              ((state->write_queue.last_rv_valid &
                BLADERF_RFIC_STATUS_WQRESULT_VALID_MASK)
               << BLADERF_RFIC_STATUS_WQRESULT_VALID_SHIFT) |

              (((uint64_t)state->write_queue.last_rv_sequence &
                BLADERF_RFIC_STATUS_WQSEQUENCE_MASK)
               << BLADERF_RFIC_STATUS_WQSEQUENCE_SHIFT) |

              ((state->init_stage & BLADERF_RFIC_STATUS_STAGE_MASK)
               << BLADERF_RFIC_STATUS_STAGE_SHIFT);

    if (active != NULL && active->state != ENTRY_STATE_INVALID) {
        *status |= UINT64_C(1) << BLADERF_RFIC_STATUS_ACTIVE_VALID_SHIFT;
        *status |= ((uint64_t)active->cmd &
                    BLADERF_RFIC_STATUS_ACTIVE_CMD_MASK)
                   << BLADERF_RFIC_STATUS_ACTIVE_CMD_SHIFT;
        *status |= ((uint64_t)active->ch &
                    BLADERF_RFIC_STATUS_ACTIVE_CH_MASK)
                   << BLADERF_RFIC_STATUS_ACTIVE_CH_SHIFT;
        *status |= ((uint64_t)active->state &
                    BLADERF_RFIC_STATUS_ACTIVE_STATE_MASK)
                   << BLADERF_RFIC_STATUS_ACTIVE_STATE_SHIFT;
    }

    return true;
}

/* Read-only transaction identity is separate from STATUS so its 32-bit
 * retirement sequence and active queue fields retain their existing layout.
 * [15:0]=active ID, [31:16]=last retired ID, [63:32]=retire sequence. */
bool _rfic_cmd_rd_transaction_status(struct rfic_state *state,
                                     bladerf_channel channel,
                                     uint64_t *status)
{
    struct rfic_queue_entry *active =
        rfic_queue_peek(&state->write_queue);
    uint16_t active_id = active == NULL ? 0 : active->transaction_id;

    *status = (uint64_t)active_id |
              ((uint64_t)state->write_queue.last_rv_transaction_id << 16) |
              ((uint64_t)state->write_queue.last_rv_sequence << 32);
    return true;
}

/* Diagnostic heartbeat for a tagged active callback. The counter advances
 * only at cooperative safe points (delays, SPI/AXI completion) and on entry /
 * return from the callback. It proves that firmware reached a boundary; it
 * does not imply hardware calibration completion or cancellation. */
bool _rfic_cmd_rd_progress(struct rfic_state *state,
                           bladerf_channel channel,
                           uint64_t *status)
{
    struct rfic_queue_entry *active =
        rfic_queue_peek(&state->write_queue);
    uint16_t transaction_id = active == NULL
                                  ? state->write_queue.last_rv_transaction_id
                                  : active->transaction_id;
    uint8_t command = active == NULL
                          ? state->write_queue.last_rv_cmd
                          : active->cmd;
    uint8_t phase_or_result = active == NULL
                                  ? state->write_queue.last_result_code
                                  : (uint8_t)active->state;

    /* A heartbeat proves that NIOS reached a cooperative safe point, but by
     * itself it cannot distinguish a long calibration from a stuck callback.
     * Report the retained sub-transaction phase in the existing phase byte
     * whenever one owns the RFIC. This is diagnostic only: it does not imply
     * hardware completion and does not alter command retirement semantics. */
    if (active != NULL && active->state == ENTRY_STATE_RUNNING) {
        if (state->rfpll_job_pending) {
            phase_or_result = bladerf_rfic_progress_phase_encode(
                BLADERF_RFIC_PROGRESS_PHASE_RFPLL,
                state->job_context.rfpll_job.rate_transaction.stage);
        } else if (active->cmd == BLADERF_RFIC_COMMAND_INIT) {
            phase_or_result = bladerf_rfic_progress_phase_encode(
                BLADERF_RFIC_PROGRESS_PHASE_INIT,
                _rfic_init_progress_step(state));
        } else if (state->rfdc_job_pending) {
            phase_or_result = bladerf_rfic_progress_phase_encode(
                BLADERF_RFIC_PROGRESS_PHASE_RFDC,
                state->job_context.rfdc_job.stage);
        } else if (state->tx_quad_job_pending) {
            phase_or_result = bladerf_rfic_progress_phase_encode(
                BLADERF_RFIC_PROGRESS_PHASE_TXQ,
                state->job_context.tx_quad_job.stage);
        } else if (state->sample_rate_job_pending) {
            phase_or_result = bladerf_rfic_progress_phase_encode(
                BLADERF_RFIC_PROGRESS_PHASE_SAMPLE_RATE,
                state->job_context.sample_rate_job.stage);
        } else if (state->bandwidth_job_pending) {
            phase_or_result = bladerf_rfic_progress_phase_encode(
                BLADERF_RFIC_PROGRESS_PHASE_BANDWIDTH,
                state->job_context.bandwidth_job.stage);
        } else if (state->fir_enable_job_pending) {
            phase_or_result = bladerf_rfic_progress_phase_encode(
                BLADERF_RFIC_PROGRESS_PHASE_FIR,
                state->job_context.fir_enable_job.stage);
        }
    }
    struct bladerf_rfic_progress progress = {
        .sequence = state->write_queue.progress_sequence,
        .transaction_id = transaction_id,
        .command = command,
        .phase_or_result = phase_or_result,
    };

    (void)channel;
    *status = bladerf_rfic_progress_pack(&progress);
    return true;
}

/* Extended terminal detail is read-only and bound to the same transaction ID
 * and retirement sequence as PROGRESS/RESULT_ACK. Never expose toolchain errno
 * values on the wire; both failure fields are stable result classes. */
bool _rfic_cmd_rd_result_detail(struct rfic_state *state,
                                bladerf_channel channel,
                                uint64_t *status)
{
    struct rfic_queue const *q = &state->write_queue;
    struct bladerf_rfic_result_detail detail = {
        .sequence = q->last_rv_sequence,
        .transaction_id = q->last_rv_valid
                              ? q->last_rv_transaction_id : 0,
        .primary_result = q->last_rv_valid
                              ? q->last_result_code
                              : BLADERF_RFIC_RESULT_NO_RESULT,
        .cleanup_result = q->last_rv_valid
                              ? q->last_cleanup_result_code
                              : BLADERF_RFIC_RESULT_NO_RESULT,
    };

    *status = bladerf_rfic_result_detail_pack(&detail);
    return true;
}

/**
 * @brief      Initialize the RFIC
 *
 * @param[in]  init_state   bladerf_rfic_init_state
 *
 * @return     true if successful, false if not
 */
bool _rfic_cmd_wr_init(struct rfic_state *state,
                       bladerf_channel channel,
                       uint32_t init_state)
{
    bladerf_rfic_init_state new_state = (bladerf_rfic_init_state)init_state;

    /* OFF is also the fail-closed state for a failed standby transition.
     * That path may retain the PHY or fail to clear RFFE controls, so an
     * explicit OFF request must retry teardown instead of treating the
     * state label as proof that cleanup completed. */
    if (state->init_state == new_state &&
        new_state != BLADERF_RFIC_INIT_STATE_OFF) {
        DBG("%s: already in state %x\n", __FUNCTION__, state->init_state);
        return true;
    }

    switch (new_state) {
        case BLADERF_RFIC_INIT_STATE_OFF:
            return _rfic_deinitialize(state, false, false);

        case BLADERF_RFIC_INIT_STATE_ON:
            if (state->calibration_faulted) {
                /* An ambiguous calibration may still be in flight. Assert
                 * board RESETB before releasing its PHY/resources; no AD9361
                 * SPI is issued until the fresh initialization begins. */
                if (!_clear_rffe_ctrl()) {
                    RFIC_ERR("failed to disconnect RFFE before RESETB", -EIO);
                    return false;
                }
                if (!_reset_rfic(true)) {
                    RFIC_ERR("failed to assert AD9361 RESETB", -EIO);
                    return false;
                }
                if (!_rfic_deinitialize(state, true, false)) {
                    DBG("%s: teardown failed before calibration recovery\n",
                        __FUNCTION__);
                    return false;
                }
                /* Hold the chip in reset only for the documented minimum;
                 * release it before any AXI access because the AD9361
                 * interface clock is derived from the transceiver. */
                no_os_udelay(2);
                if (!_reset_rfic(false)) {
                    RFIC_ERR("failed to release AD9361 RESETB", -EIO);
                    DBG("%s: failed to release AD9361 RESETB\n",
                        __FUNCTION__);
                    return false;
                }
                /* UG-570 requires at least 1 us after RESETB rises before
                 * programming. Give the interface clock additional startup
                 * margin before ad9361_init() touches AXI. */
                no_os_mdelay(1);
            }

            if (_rfic_initialize(state)) {
                state->calibration_faulted = false;
                return true;
            }

            /* Any failed initialization can leave partially programmed RF
             * state, whether its partial PHY was consumed or retained. Keep a
             * board-level fault so the next ON resets before retrying. */
            state->calibration_faulted = true;
            if (!_rfic_deinitialize(state, false, true)) {
                DBG("%s: cleanup after failed initialization also failed\n",
                    __FUNCTION__);
            }
            return false;

        case BLADERF_RFIC_INIT_STATE_STANDBY:
            return _rfic_standby(state);
    }

    RFIC_ERR("invalid RFIC initialization state", -EINVAL);
    return false;
}

/**
 * @brief       Query RFIC initialization state
 *
 * @param[out]  init_state  bladerf_rfic_init_state value, casted to uint32_t
 *
 * @return      true if successful, false if not
 */
bool _rfic_cmd_rd_init(struct rfic_state *state,
                       bladerf_channel channel,
                       uint32_t *init_state)
{
    *init_state = (uint32_t)state->init_state;

    return true;
}

/**
 * @brief       Set the enabled status for a given channel
 *
 * @param[in]   channel  Channel
 * @param[in]   enable     0 to disable, >= 1 to enable
 *
 * @return      true if successful, false otherwise
 */
bool _rfic_cmd_wr_enable(struct rfic_state *state,
                         bladerf_channel channel,
                         uint32_t enable)
{
    bladerf_direction direction =
        BLADERF_CHANNEL_IS_TX(channel) ? BLADERF_TX : BLADERF_RX;
    uint32_t reg;     /* RFFE register value */
    bool ch_enabled;  /* Channel: initial state */
    bool ch_enable;   /* Channel: target state */
    bool ch_pending;  /* Channel: target state is not initial state */
    bool dir_enabled; /* Direction: initial state */
    bool dir_enable;  /* Direction: target state */
    bool dir_pending; /* Direction: target state is not initial state */

    struct band_port_map const *port_map;
    bladerf_channel subch;
    bladerf_frequency freq = 0;
    size_t i;

    if (state->rfdc_resume_enable_command) {
        bool continuation_matches =
            state->rfdc_resume_enable_channel == channel &&
            state->rfdc_resume_enable_value == (enable > 0);
        bool commit_allowed = rfic_rx_enable_resume_commit_allowed(
            state->rfdc_resume_enable_status, continuation_matches,
            state->phy != NULL &&
                !ad9361_calibration_state_unknown(state->phy));
        uint32_t resume_reg = state->rfdc_resume_enable_rffe_reg;

        state->rfdc_resume_enable_command = false;
        state->rfdc_resume_enable_status = 0;
        if (!commit_allowed) {
            _rfic_fail_closed_after_path_mutation(
                state, "RX enable RFDC continuation failed or mismatched");
            return false;
        }
        if (!rffe_csr_write_checked(resume_reg, RFFE_CSR_OUTPUT_MASK)) {
            _rfic_fail_closed_after_path_mutation(
                state, "RFFE enable commit failed after RX RFDC");
            return false;
        }
        return true;
    }

    if (state->tx_quad_resume_command) {
        /* TXQ has completed and restored the AD9361 transaction. Continue at
         * the single deferred board commit; do not replay RFIC port setup,
         * register reads, or calibration initiation. */
        if (!rfic_tx_enable_continuation_matches(
                state->tx_quad_resume_channel,
                state->tx_quad_resume_value, channel, enable)) {
            state->tx_quad_resume_command = false;
            _rfic_fail_closed_after_path_mutation(
                state, "TX enable continuation identity mismatch");
            return false;
        }

        reg = state->tx_quad_resume_rffe_reg;
        state->tx_quad_resume_command = false;
        if (!rffe_csr_write_checked(reg, RFFE_CSR_OUTPUT_MASK)) {
            _rfic_fail_closed_after_path_mutation(
                state, "RFFE enable commit failed after TXQ");
            return false;
        }
        return true;
    }

    /* Read RFFE control register */
    if (!rffe_csr_read_checked(&reg)) {
        RFIC_ERR("RFFE read before enable-state update", -EIO);
        return false;
    }

#if 0
    uint32_t reg_old = reg;
#endif

    /* Pre-compute useful values */
    /* Calculate initial and target states */
    ch_enabled  = _rffe_ch_enabled(reg, channel);
    dir_enabled = _rffe_dir_enabled(reg, direction);
    ch_enable   = (enable > 0);
    dir_enable  = ch_enable || _rffe_dir_otherwise_enabled(reg, channel);
    ch_pending  = ch_enabled != ch_enable;
    dir_pending = dir_enabled != dir_enable;

    if (!ch_pending && !dir_pending) {
        DBG("%s: nothing to do?\n", __FUNCTION__);
        return true;
    }

    /* Query the current frequency if we'll need it */
    if (ch_enable) {
        if (BLADERF_CHANNEL_IS_TX(channel)) {
            CHECK_BOOL(ad9361_get_tx_lo_freq(state->phy, &freq));
        } else {
            CHECK_BOOL(ad9361_get_rx_lo_freq(state->phy, &freq));
        }
    }

    /* Channel Setup/Teardown */
    if (ch_pending) {
        /* Modify SPDT bits */
        CHECK_BOOL(_modify_spdt_bits_by_freq(&reg, channel, ch_enable, freq));

        /* Modify MIMO channel enable bits */
        if (ch_enable) {
            reg |= (1 << _get_rffe_control_bit_for_ch(channel));
        } else {
            reg &= ~(1 << _get_rffe_control_bit_for_ch(channel));
        }
    }

    /* Direction Setup/Teardown */
    if (dir_pending) {
        /* Modify ENABLE/TXNRX bits */
        if (dir_enable) {
            reg |= (1 << _get_rffe_control_bit_for_dir(direction));
        } else {
            FOR_EACH_CHANNEL(direction, 2, i, subch)
            {
                CHECK_BOOL(_modify_spdt_bits_by_freq(&reg, subch, false, 0));
            }

            reg &= ~(1 << _get_rffe_control_bit_for_dir(direction));
        }

        /* Select RFIC port */
        /* Look up the port configuration for this frequency */
        port_map = _get_band_port_map_by_freq(channel, ch_enable ? freq : 0);
        if (NULL == port_map) {
            RFIC_ERR("no RF port mapping for requested frequency", -ERANGE);
            return false;
        }

        /* Set the AD9361 port accordingly */
        if (BLADERF_CHANNEL_IS_TX(channel)) {
            if (ad9361_set_tx_rf_port_output(
                    state->phy, port_map->rfic_port) < 0) {
                _rfic_fail_closed_after_path_mutation(
                    state, "AD9361 TX port update failed");
                return false;
            }
        } else {
            if (ad9361_set_rx_rf_port_input(
                    state->phy, port_map->rfic_port) < 0) {
                _rfic_fail_closed_after_path_mutation(
                    state, "AD9361 RX port update failed");
                return false;
            }
            if (ch_enable) {
                /* Enabling RX can select a different AD9361 input without
                 * a corresponding LO write. Calibrate that input before
                 * committing the RFFE enable so its own RFDC correction is
                 * present when samples resume. */
                if (!_rfic_ensure_rx_rfdc_calibrated(state, freq)) {
                    _rfic_fail_closed_after_path_mutation(
                        state, "RX RFDC calibration failed after port update");
                    return false;
                }
                if (state->rfdc_job_pending) {
                    state->rfdc_commit_rx_frequency = false;
                    state->rfdc_resume_enable_command = true;
                    state->rfdc_resume_enable_channel = channel;
                    state->rfdc_resume_enable_value = ch_enable;
                    state->rfdc_resume_enable_rffe_reg = reg;
                    state->rfdc_resume_enable_status = 1;
                    return true;
                }
            }
        }
    }

    /* Do not publish the TX enable until calibration history is valid for
     * the final RFIC TXA/TXB selector and current LO. */
    if (ch_enable && BLADERF_CHANNEL_IS_TX(channel)) {
        bool tx_quad_pending;

        if (!rfic_start_tx_quad_calibration_for_command(
                &tx_quad_pending, NULL)) {
            _rfic_fail_closed_after_path_mutation(
                state, "TX quadrature calibration failed before RFFE enable");
            return false;
        }
        if (tx_quad_pending) {
            /* The NIOS worker retains this queue entry and RFIC ownership.
             * Save the exact post-calibration commit; its continuation must
             * not replay the earlier RFIC port mutations. */
            state->tx_quad_resume_channel = channel;
            state->tx_quad_resume_value = ch_enable;
            state->tx_quad_resume_rffe_reg = reg;
            return true;
        }
    }

#if 0
    DBG("%s: ch=%x dir=%x ch_en=%x ch_pend=%x dir_en=%x dir_pend=%x "
        "reg=%x->%x\n",
        __FUNCTION__, channel, direction, ch_enable, ch_pending, dir_enable,
        dir_pending, reg_old, reg);
#endif

    /* The AD9361 port selector is global and was already changed above. A
     * failed final RFFE commit therefore leaves the effective RF path
     * ambiguous even if the AD9361 SPI writes themselves all succeeded. */
    if (!rffe_csr_write_checked(reg, RFFE_CSR_OUTPUT_MASK)) {
        _rfic_fail_closed_after_path_mutation(
            state, "RFFE enable state failed readback after RFIC port update");
        return false;
    }

    return true;
}

/**
 * @brief      Get the enabled status for a given channel
 *
 * @param[in]  channel  The channel
 * @param[out] enabled  0 if disabled, 1 if enabled
 *
 * @return     true if successful, false otherwise
 */
bool _rfic_cmd_rd_enable(struct rfic_state *state,
                         bladerf_channel channel,
                         uint32_t *enabled)
{
    uint32_t reg;

    if (!rffe_csr_read_checked(&reg)) {
        RFIC_ERR("RFFE read for enable-state query", -EIO);
        return false;
    }
    *enabled = _rffe_ch_enabled(reg, channel) ? 1 : 0;

    return true;
}

/**
 * @brief      Set sample rate for a given channel
 *
 * @param[in]  channel    The channel
 * @param[in]  rate       The rate
 *
 * @return     true if successful, false if not
 */
bool _rfic_cmd_wr_samplerate(struct rfic_state *state,
                             bladerf_channel channel,
                             uint32_t rate)
{
    bool converter_available = false;
    bool resumed = state->sample_rate_resume_command;
    int32_t status;
    int32_t cleanup_status;

    if (resumed) {
        state->sample_rate_resume_command = false;
    } else {
        for (unsigned int i = 0; i < NUM_MODULES; i++) {
            state->sample_rate_saved_samplerate_invalid[i] =
                state->samplerate_invalid[i];
            state->sample_rate_saved_bandwidth_invalid[i] =
                state->bandwidth_invalid[i];
        }

        status = ad9361_sample_rate_transaction_start(
            state->phy, rate, BLADERF_CHANNEL_IS_TX(channel),
            &state->job_context.sample_rate_job);
        if (status < 0) {
            enum rfic_sample_rate_cache_action cache_action =
                rfic_sample_rate_cache_action(
                    state->job_context.sample_rate_job.active, true,
                    state->phy->current_bw_valid &&
                    !ad9361_calibration_state_unknown(state->phy));

            if (cache_action == RFIC_SAMPLE_RATE_CACHE_INVALIDATE) {
                rfic_cache_invalidate_all(state->samplerate_invalid,
                                          NUM_MODULES);
                rfic_cache_invalidate_all(state->bandwidth_invalid,
                                          NUM_MODULES);
            }
            rfic_command_record_error(status);
            return false;
        }
        if (state->job_context.sample_rate_job.active) {
            /* RX/TX share the AD9361 clock tree. Close both readbacks once
             * the plan has mutated and keep them closed through TXQ, restore
             * and final transaction completion. */
            rfic_cache_invalidate_all(state->samplerate_invalid,
                                      NUM_MODULES);
            rfic_cache_invalidate_all(state->bandwidth_invalid,
                                      NUM_MODULES);
            state->sample_rate_job_pending = true;
            return true;
        }
    }

    if (!BLADERF_CHANNEL_IS_TX(channel)) {

        /* Only a PHY with an AXI ADC handle can execute the BIST timing
         * sweep. The NIOS-owned PHY intentionally omits that handle and uses
         * preset delays (skip mode 2); calling ad9361_dig_tune() anyway
         * fails validation before touching the RFIC and rejects every RX
         * sample-rate update. Keep an explicit failure for calibration modes
         * that require a tuner but have no converter owner. */
#ifndef AXI_ADC_NOT_PRESENT
        converter_available = state->phy->rx_adc != NULL;
#endif
        if (converter_available) {
            cleanup_status = 0;
            status = ad9361_dig_tune_with_cleanup(
                state->phy, 61440000, DO_IDELAY | DO_ODELAY,
                &cleanup_status);
            rfic_command_record_cleanup_error(cleanup_status);
            if (status < 0) {
                RFIC_ERR("ad9361_dig_tune", status);
                return false;
            }
        } else if (state->phy->pdata->dig_interface_tune_skipmode != 2) {
            RFIC_ERR("digital interface tune unavailable without AXI ADC",
                     -ENOTSUP);
            DBG("%s: digital interface tuning requires an AXI ADC handle\n",
                __FUNCTION__);
            return false;
        }
    }

    rfic_cache_validate_all(state->samplerate_invalid, NUM_MODULES);
    rfic_cache_validate_all(state->bandwidth_invalid, NUM_MODULES);
    return true;
}

/**
 * @brief      Get sample rate for a given channel
 *
 * @param[in]  channel    The channel
 * @param[out] rate       The sample rate
 *
 * @return     true if successful, false if not
 */
bool _rfic_cmd_rd_samplerate(struct rfic_state *state,
                             bladerf_channel channel,
                             uint32_t *rate)
{
    bladerf_direction direction =
        BLADERF_CHANNEL_IS_TX(channel) ? BLADERF_TX : BLADERF_RX;

    if (!rfic_cache_read_is_valid(state->samplerate_invalid, NUM_MODULES,
                                  direction)) {
        RFIC_ERR("sample-rate cache is invalid", -EIO);
        return false;
    }

    if (BLADERF_CHANNEL_IS_TX(channel)) {
        CHECK_BOOL(ad9361_get_tx_sampling_freq(state->phy, rate));
    } else {
        CHECK_BOOL(ad9361_get_rx_sampling_freq(state->phy, rate));
    }

    return true;
}

/**
 * @brief      Set LO frequency for a given channel
 *
 * @param[in]  channel    The channel
 * @param[in]  frequency  The frequency
 *
 * @return     true if successful, false if not
 */
static bool _rfic_cmd_wr_frequency_internal(struct rfic_state *state,
                                            bladerf_channel channel,
                                            uint64_t frequency,
                                            bool init_sequence)
{
    bladerf_direction dir =
        BLADERF_CHANNEL_IS_TX(channel) ? BLADERF_TX : BLADERF_RX;
    bladerf_channel port_ch = channel;
    bladerf_channel other_ch;
    uint32_t reg;
    size_t i;
    bool resumed_rfpll = state->frequency_rfpll_resume_command;

    /* A failed port, LO, or calibration step must not leave the cached RFIC
     * frequency readable as a successful transition. Publish validity only
     * after the complete direction-specific operation below succeeds. */
    /* During cold initialization ADI starts without cached LO rates; the
     * explicit init sequence establishes both starting synthesizers. No
     * runtime command may opt out of the peer-frequency check. */
    if (rfic_frequency_change_blocked(
            init_sequence,
            rfic_frequency_peer_unknown(
                BLADERF_CHANNEL_IS_TX(channel)
                    ? BLADERF_MODULE_TX : BLADERF_MODULE_RX))) {
        RFIC_ERR("peer LO frequency is unknown", -EINVAL);
        return false;
    }
    state->frequency_invalid[dir] = true;

    if (resumed_rfpll) {
        if (state->frequency_rfpll_channel != channel ||
            state->frequency_rfpll_target != frequency ||
            state->frequency_rfpll_init_sequence != init_sequence) {
            state->frequency_rfpll_resume_command = false;
            RFIC_ERR("RFPLL continuation does not match active command",
                     -EINVAL);
            return false;
        }
        state->frequency_rfpll_resume_command = false;
        if (state->frequency_rfpll_status < 0) {
            RFIC_ERR("RFPLL clock transaction",
                     state->frequency_rfpll_status);
            _rfic_fail_closed_after_path_mutation(
                state, "LO update failed after RF path preselection");
            return false;
        }
        goto lo_programmed;
    }

    /* Scheduled fastlock recalls bypass ADI's recall API. Exit that external
     * mode explicitly before the ordinary LO setter relies on current_profile
     * to run its ALC/VCO cleanup sequence. */
    {
        int32_t status = ad9361_fastlock_exit_foreign(
            state->phy, BLADERF_CHANNEL_IS_TX(channel));
        if (status < 0) {
            RFIC_ERR("ad9361_fastlock_exit_foreign", status);
            return false;
        }
    }

    if (!rffe_csr_read_checked(&reg)) {
        RFIC_ERR("RFFE read before frequency update", -EIO);
        return false;
    }

    /* We have to do this for all the channels sharing the same LO. */
    FOR_EACH_CHANNEL(dir, 2, i, other_ch)
    {
        /* Is this channel enabled? */
        bool enable = _rffe_ch_enabled(reg, other_ch);

        /* Update SPDT bits accordingly */
        CHECK_BOOL(
            _modify_spdt_bits_by_freq(&reg, other_ch, enable, frequency));
        if (enable) {
            port_ch = other_ch;
        }
    }

    CHECK_TRUE(rffe_csr_write_checked(reg, RFFE_CSR_OUTPUT_MASK));

    /* Preselect the target TX output even when TX is currently disabled.
     * The finalizer below must key calibration to the path the next enable
     * will use; RX still defers selector changes until its enable path. */
    CHECK_BOOL(set_ad9361_port_by_freq(
        state->phy, port_ch,
        BLADERF_CHANNEL_IS_TX(channel) || _rffe_ch_enabled(reg, port_ch),
        frequency));

    {
        uint32_t source = BLADERF_CHANNEL_IS_TX(channel)
            ? TX_RFPLL : RX_RFPLL;
        int32_t status = ad9361_rfpll_clock_transaction_start(
            state->phy, state->phy->ref_clk_scale[source],
            ad9361_to_clk(frequency), &state->job_context.rfpll_job);

        /* External-LO callbacks do not yet expose a retained contract. Keep
         * their established path explicit; the xA4 NIOS configuration uses
         * the internal synthesizers and always takes the retained path. */
        if (status == -ENOTSUP) {
            status = BLADERF_CHANNEL_IS_TX(channel)
                ? ad9361_set_tx_lo_freq(state->phy, frequency)
                : ad9361_set_rx_lo_freq(state->phy, frequency);
        }
        if (status < 0) {
            RFIC_ERR("RFPLL clock transaction start", status);
            _rfic_fail_closed_after_path_mutation(
                state, "LO update failed after RF path preselection");
            return false;
        }
        if (state->job_context.rfpll_job.active) {
            state->rfpll_job_pending = true;
            state->frequency_rfpll_channel = channel;
            state->frequency_rfpll_target = frequency;
            state->frequency_rfpll_init_sequence = init_sequence;
            state->frequency_rfpll_status = 0;
            return true;
        }
    }

lo_programmed:
    if (BLADERF_CHANNEL_IS_TX(channel)) {
        bool tx_quad_pending;

        CHECK_TRUE(rfic_start_tx_quad_calibration_for_frequency(
            &tx_quad_pending, NULL));
        if (tx_quad_pending)
            return true;
    } else {
        /* RFDC history is indexed by the actual AD9361 input selector, not
         * frequency alone. */
        CHECK_TRUE(_rfic_ensure_rx_rfdc_calibrated(state, frequency));
        if (state->rfdc_job_pending) {
            state->rfdc_commit_rx_frequency = true;
            return true;
        }
    }

    state->frequency_invalid[dir] = false;
    return true;
}

bool _rfic_cmd_wr_frequency(struct rfic_state *state,
                            bladerf_channel channel,
                            uint64_t frequency)
{
    return _rfic_cmd_wr_frequency_internal(state, channel, frequency, false);
}

/**
 * @brief      Get LO frequency for a given channel
 *
 * @param[in]  channel    The channel
 * @param[out] frequency  The frequency
 *
 * @return     true if successful, false if not
 */
bool _rfic_cmd_rd_frequency(struct rfic_state *state,
                            bladerf_channel channel,
                            uint64_t *frequency)
{
    if (BLADERF_CHANNEL_IS_TX(channel)) {
        if (state->frequency_invalid[BLADERF_TX]) {
            RFIC_ERR("TX LO frequency is invalid", -EIO);
            return false;
        }
        CHECK_BOOL(ad9361_get_tx_lo_freq(state->phy, frequency));
    } else {
        if (state->frequency_invalid[BLADERF_RX]) {
            RFIC_ERR("RX LO frequency is invalid", -EIO);
            return false;
        }
        CHECK_BOOL(ad9361_get_rx_lo_freq(state->phy, frequency));
    }

    return true;
}

/**
 * @brief      Set bandwidth
 *
 * @param[in]  channel    The channel
 * @param[in]  bandwidth  The bandwidth
 *
 * @return     true if successful, false if not
 */
bool _rfic_cmd_wr_bandwidth(struct rfic_state *state,
                            bladerf_channel channel,
                            uint32_t bandwidth)
{
    bool resumed = state->bandwidth_resume_command;
    bool bandwidth_cache_invalid =
        state->bandwidth_invalid[BLADERF_RX] ||
        state->bandwidth_invalid[BLADERF_TX];
    uint32_t rx_bandwidth, tx_bandwidth;
    int32_t status;

    if (resumed) {
        state->bandwidth_resume_command = false;
        rfic_cache_validate_all(state->bandwidth_invalid, NUM_MODULES);
        return true;
    }

    rx_bandwidth = BLADERF_CHANNEL_IS_TX(channel)
                       ? state->phy->current_rx_bw_Hz
                       : ad9361_validate_rf_bw(state->phy, bandwidth);
    tx_bandwidth = BLADERF_CHANNEL_IS_TX(channel)
                       ? ad9361_validate_rf_bw(state->phy, bandwidth)
                       : state->phy->current_tx_bw_Hz;

    /* Preserve the old no-op behavior when the requested pair is already
     * committed and both NIOS readback caches are known. */
    if (!bandwidth_cache_invalid && state->phy->current_bw_valid &&
        rx_bandwidth == state->phy->current_rx_bw_Hz &&
        tx_bandwidth == state->phy->current_tx_bw_Hz) {
        return true;
    }

    /* ADI updates both analog filter chains and may run TXQ. Keep this queue
     * entry resident while the operation yields between completion polls. */
    status = ad9361_rf_bandwidth_transaction_start(
        state->phy, rx_bandwidth, tx_bandwidth, &state->job_context.bandwidth_job);
    if (status < 0) {
        if (!state->phy->current_bw_valid ||
            ad9361_calibration_state_unknown(state->phy)) {
            rfic_cache_invalidate_all(state->bandwidth_invalid, NUM_MODULES);
        }
        rfic_command_record_error(status);
        return false;
    }
    if (state->job_context.bandwidth_job.active) {
        rfic_cache_invalidate_all(state->bandwidth_invalid, NUM_MODULES);
        state->bandwidth_job_pending = true;
        return true;
    }

    rfic_cache_validate_all(state->bandwidth_invalid, NUM_MODULES);

    return true;
}

/**
 * @brief      Get bandwidth
 *
 * @param[in]  channel    The channel
 * @param[out] bandwidth  The bandwidth
 *
 * @return     true if successful, false if not
 */
bool _rfic_cmd_rd_bandwidth(struct rfic_state *state,
                            bladerf_channel channel,
                            uint32_t *bandwidth)
{
    bladerf_direction direction =
        BLADERF_CHANNEL_IS_TX(channel) ? BLADERF_TX : BLADERF_RX;

    if (!rfic_cache_read_is_valid(state->bandwidth_invalid, NUM_MODULES,
                                  direction)) {
        RFIC_ERR("bandwidth cache is invalid", -EIO);
        return false;
    }

    if (BLADERF_CHANNEL_IS_TX(channel)) {
        CHECK_BOOL(ad9361_get_tx_rf_bandwidth(state->phy, bandwidth));
    } else {
        CHECK_BOOL(ad9361_get_rx_rf_bandwidth(state->phy, bandwidth));
    }

    return true;
}

/**
 * @brief      Set gainmode
 *
 * @param[in]  channel    The channel
 * @param[in]  gainmode   The gainmode
 *
 * @return     true if successful, false if not
 */
bool _rfic_cmd_wr_gainmode(struct rfic_state *state,
                           bladerf_channel channel,
                           uint32_t gainmode)
{
    uint8_t const rfic_ch = channel >> 1;
    enum rf_gain_ctrl_mode gc_mode;

    /* Mode conversion */
    switch (gainmode) {
        case BLADERF_GAIN_DEFAULT:
            switch (channel) {
                case BLADERF_CHANNEL_RX(0):
                    gc_mode = bladerf2_rx_gain_mode_default[0];
                    break;

                case BLADERF_CHANNEL_RX(1):
                    gc_mode = bladerf2_rx_gain_mode_default[1];
                    break;

                default:
                    RFIC_ERR("invalid channel for default gain mode",
                             -EINVAL);
                    return false;
            }
            break;
        case BLADERF_GAIN_MGC:
            gc_mode = RF_GAIN_MGC;
            break;
        case BLADERF_GAIN_FASTATTACK_AGC:
            gc_mode = RF_GAIN_FASTATTACK_AGC;
            break;
        case BLADERF_GAIN_SLOWATTACK_AGC:
            gc_mode = RF_GAIN_SLOWATTACK_AGC;
            break;
        case BLADERF_GAIN_HYBRID_AGC:
            gc_mode = RF_GAIN_HYBRID_AGC;
            break;
        default:
            RFIC_ERR("invalid gain mode", -EINVAL);
            return false;
    }

    /* Set the mode! */
    CHECK_BOOL(ad9361_set_rx_gain_control_mode(state->phy, rfic_ch, gc_mode));

    return true;
}

/**
 * @brief      Get gainmode
 *
 * @param[in]  channel   The channel
 * @param[out] gainmode  The gainmode
 *
 * @return     true if successful, false if not
 */
bool _rfic_cmd_rd_gainmode(struct rfic_state *state,
                           bladerf_channel channel,
                           uint32_t *gainmode)
{
    uint8_t const rfic_ch = channel >> 1;
    uint8_t gc_mode;

    /* Get the gain mode */
    CHECK_BOOL(ad9361_get_rx_gain_control_mode(state->phy, rfic_ch, &gc_mode));

    /* Mode conversion */
    switch (gc_mode) {
        case RF_GAIN_MGC:
            *gainmode = BLADERF_GAIN_MGC;
            break;
        case RF_GAIN_FASTATTACK_AGC:
            *gainmode = BLADERF_GAIN_FASTATTACK_AGC;
            break;
        case RF_GAIN_SLOWATTACK_AGC:
            *gainmode = BLADERF_GAIN_SLOWATTACK_AGC;
            break;
        case RF_GAIN_HYBRID_AGC:
            *gainmode = BLADERF_GAIN_HYBRID_AGC;
            break;
        default:
            RFIC_ERR("unknown RFIC gain mode readback", -EIO);
            return false;
    }

    return true;
}

/**
 * @brief      Set gain
 *
 * expects values that are ready to be passed down to the device.
 * e.g.:
 *  TX: mdB attenuation (-10 dB gain -> gain = 10000)
 *  RX: dB gain (10 dB gain -> gain = 10)
 * Values may be negative, and the uint32_t will be casted appropriately
 *
 * @param[in]  channel    The channel
 * @param[in]  gain       The gain
 *
 * @return     true if successful, false if not
 */
bool _rfic_cmd_wr_gain(struct rfic_state *state,
                       bladerf_channel channel,
                       uint32_t gain)
{
    uint8_t const rfic_ch = channel >> 1;

    if (BLADERF_CHANNEL_IS_TX(channel)) {
        bool muted;

        CHECK_BOOL(txmute_get(state->phy, channel, &muted));

        if (muted) {
            CHECK_BOOL(txmute_set_cached(state->phy, channel, gain));
        } else {
            CHECK_BOOL(ad9361_set_tx_attenuation(state->phy, rfic_ch, gain));
        }
    } else {
        CHECK_BOOL(ad9361_set_rx_rf_gain(state->phy, rfic_ch, (int32_t)gain));
    }

    return true;
}

/**
 * @brief       Get gain
 *
 * returns values that are direct from the device.
 * e.g.:
 *  TX: mdB attenuation (data = 10000 -> -10 dB gain)
 *  RX: dB gain (data = 10 -> 10 dB gain)
 * Values may be negative, and will be casted to uint32_t
 *
 *
 * @param[in]   channel    The channel
 * @param[out]  gain       The gain
 *
 * @return      true if successful, false if not
 */
bool _rfic_cmd_rd_gain(struct rfic_state *state,
                       bladerf_channel channel,
                       uint32_t *gain)
{
    uint8_t const rfic_ch = channel >> 1;

    if (BLADERF_CHANNEL_IS_TX(channel)) {
        bool muted;

        CHECK_BOOL(txmute_get(state->phy, channel, &muted));

        if (muted) {
            *gain = txmute_get_cached(state->phy, channel);
        } else {
            CHECK_BOOL(ad9361_get_tx_attenuation(state->phy, rfic_ch, gain));
        }
    } else {
        struct rf_rx_gain rx_gain;

        CHECK_BOOL(ad9361_get_rx_gain(state->phy, rfic_ch + 1, &rx_gain));

        *gain = (uint32_t)rx_gain.gain_db;
    }

    return true;
}

/**
 * @brief       Get RSSI reading
 *
 * @param[in]   channel   The channel
 * @param[out]  rssi_val  RSSI, packed
 *
 * @return      true if successful, false if not
 */
bool _rfic_cmd_rd_rssi(struct rfic_state *state,
                       bladerf_channel channel,
                       uint64_t *rssi_val)
{
    uint8_t const rfic_ch = channel >> 1;
    int16_t pre, sym, mul;

    if (BLADERF_CHANNEL_IS_TX(channel)) {
        uint32_t rssi = 0;

        /* The TX power monitor gates the measurement block behind
         * REG_TX_RSSI. Without it those registers stay at zero and this
         * reports 0 dBm at every gain setting, which reads as "no
         * output" rather than "not measured". */
        CHECK_BOOL(
            ad9361_txmon_enable(state->phy, (0 == rfic_ch) ? TX_1 : TX_2));

        CHECK_BOOL(ad9361_get_tx_rssi(state->phy, rfic_ch, &rssi));

        mul = -1000;
        pre = rssi;
        sym = rssi;
    } else {
        struct rf_rssi rssi;

        CHECK_BOOL(ad9361_get_rx_rssi(state->phy, rfic_ch, &rssi));

        mul = -rssi.multiplier;
        pre = rssi.preamble;
        sym = rssi.symbol;
    }

    *rssi_val = ((uint64_t)((uint16_t)mul & BLADERF_RFIC_RSSI_MULT_MASK)
                 << BLADERF_RFIC_RSSI_MULT_SHIFT) |

                ((uint64_t)((uint16_t)pre & BLADERF_RFIC_RSSI_PRE_MASK)
                 << BLADERF_RFIC_RSSI_PRE_SHIFT) |

                ((uint64_t)((uint16_t)sym & BLADERF_RFIC_RSSI_SYM_MASK)
                 << BLADERF_RFIC_RSSI_SYM_SHIFT);

    return true;
}

/**
 * @brief       Set RFIC FIR filter
 *
 * @param[in]   channel  The channel
 * @param[in]   filter   The filter enum value
 *
 * @return      true if successful, false if not
 */
static bool _rfic_start_init_fir_pair(struct rfic_state *state,
                                     bladerf_channel channel,
                                     uint32_t filter)
{
    int32_t status = ad9361_fir_enable_pair_transaction_start(
        state->phy, 1, 1, &state->job_context.fir_enable_job);

    if (status < 0) {
        rfic_command_record_cleanup_error(
            state->job_context.fir_enable_job.cleanup_status);
        RFIC_ERR("AD9361 FIR pair enable start", status);
        return false;
    }
    if (status == 0 && state->job_context.fir_enable_job.terminal) {
        state->rxfir = state->fir_enable_init_rx_filter;
        state->txfir = state->fir_enable_init_tx_filter;
        state->filter_invalid[BLADERF_RX] = false;
        state->filter_invalid[BLADERF_TX] = false;
        return true;
    }

    state->fir_enable_job_pending = true;
    state->fir_enable_channel = channel;
    state->fir_enable_filter = filter;
    state->fir_enable_init_pair_resume = false;
    state->filter_invalid[BLADERF_RX] = true;
    state->filter_invalid[BLADERF_TX] = true;
    return true;
}

static bool _rfic_cmd_wr_filter_internal(struct rfic_state *state,
                                         bladerf_channel channel,
                                         uint32_t filter,
                                         bool init_pair)
{
    if (BLADERF_CHANNEL_IS_TX(channel)) {
        bladerf_rfic_txfir txfir       = (bladerf_rfic_txfir)filter;
        AD9361_TXFIRConfig *fir_config = NULL;
        uint8_t enable;

        if (BLADERF_RFIC_TXFIR_CUSTOM == txfir) {
            // custom FIR not implemented, assuming default
            txfir = BLADERF_RFIC_TXFIR_DEFAULT;
        }

        if (state->fir_enable_init_pair_resume) {
            if (!init_pair || state->fir_enable_channel != channel ||
                state->fir_enable_filter != filter) {
                state->fir_enable_init_pair_resume = false;
                RFIC_ERR("FIR pair continuation command mismatch", -EIO);
                return false;
            }
            state->fir_enable_init_pair_resume = false;
            state->rxfir = state->fir_enable_init_rx_filter;
            state->txfir = state->fir_enable_init_tx_filter;
            state->filter_invalid[BLADERF_RX] = false;
            state->filter_invalid[BLADERF_TX] = false;
            return true;
        }

        if (state->fir_enable_resume_command) {
            if (state->fir_enable_channel != channel ||
                state->fir_enable_filter != filter) {
                state->fir_enable_resume_command = false;
                RFIC_ERR("FIR continuation command mismatch", -EIO);
                return false;
            }
            state->fir_enable_resume_command = false;
            state->txfir = txfir;
            state->filter_invalid[BLADERF_TX] = false;
            return true;
        }

        switch (txfir) {
            case BLADERF_RFIC_TXFIR_BYPASS:
                fir_config = &bladerf2_rfic_tx_fir_config;
                enable     = 0;
                break;
            case BLADERF_RFIC_TXFIR_INT1:
                fir_config = &bladerf2_rfic_tx_fir_config;
                enable     = 1;
                break;
            case BLADERF_RFIC_TXFIR_INT2:
                fir_config = &bladerf2_rfic_tx_fir_config_int2;
                enable     = 1;
                break;
            case BLADERF_RFIC_TXFIR_INT4:
                fir_config = &bladerf2_rfic_tx_fir_config_int4;
                enable     = 1;
                break;
            default:
                RFIC_ERR("invalid TX FIR setting", -EINVAL);
                return false;
        }

        state->filter_invalid[BLADERF_TX] = true;
        {
            int32_t cleanup_status = 0;
            int32_t status = ad9361_set_tx_fir_config_with_cleanup(
                state->phy, *fir_config, &cleanup_status);

            rfic_command_record_cleanup_error(cleanup_status);
            status = rfic_operation_status_with_cleanup(
                status, cleanup_status);
            if (status < 0) {
                RFIC_ERR("AD9361 TX FIR configuration", status);
                return false;
            }
        }
        state->fir_enable_init_tx_filter = txfir;
        if (init_pair) {
            if (state->init_module_index + 1 < NUM_MODULES) {
                state->filter_invalid[BLADERF_TX] = true;
                return true;
            }
            return _rfic_start_init_fir_pair(state, channel, filter);
        }
        {
            int32_t status = ad9361_fir_enable_transaction_start(
                state->phy, true, enable, &state->job_context.fir_enable_job);

            if (status < 0) {
                rfic_command_record_cleanup_error(
                    state->job_context.fir_enable_job.cleanup_status);
                RFIC_ERR("AD9361 TX FIR enable start", status);
                return false;
            }
        }
        state->fir_enable_job_pending = true;
        state->fir_enable_channel = channel;
        state->fir_enable_filter = filter;
        state->filter_invalid[BLADERF_TX] = true;
        return true;
    } else {
        bladerf_rfic_rxfir rxfir       = (bladerf_rfic_rxfir)filter;
        AD9361_RXFIRConfig *fir_config = NULL;
        uint8_t enable;

        if (BLADERF_RFIC_RXFIR_CUSTOM == rxfir) {
            // custom FIR not implemented, assuming default
            rxfir = BLADERF_RFIC_RXFIR_DEFAULT;
        }

        if (state->fir_enable_init_pair_resume) {
            if (!init_pair || state->fir_enable_channel != channel ||
                state->fir_enable_filter != filter) {
                state->fir_enable_init_pair_resume = false;
                RFIC_ERR("FIR pair continuation command mismatch", -EIO);
                return false;
            }
            state->fir_enable_init_pair_resume = false;
            state->rxfir = state->fir_enable_init_rx_filter;
            state->txfir = state->fir_enable_init_tx_filter;
            state->filter_invalid[BLADERF_RX] = false;
            state->filter_invalid[BLADERF_TX] = false;
            return true;
        }

        if (state->fir_enable_resume_command) {
            if (state->fir_enable_channel != channel ||
                state->fir_enable_filter != filter) {
                state->fir_enable_resume_command = false;
                RFIC_ERR("FIR continuation command mismatch", -EIO);
                return false;
            }
            state->fir_enable_resume_command = false;
            state->rxfir = rxfir;
            state->filter_invalid[BLADERF_RX] = false;
            return true;
        }

        switch (rxfir) {
            case BLADERF_RFIC_RXFIR_BYPASS:
                fir_config = &bladerf2_rfic_rx_fir_config;
                enable     = 0;
                break;
            case BLADERF_RFIC_RXFIR_DEC1:
                fir_config = &bladerf2_rfic_rx_fir_config;
                enable     = 1;
                break;
            case BLADERF_RFIC_RXFIR_DEC2:
                fir_config = &bladerf2_rfic_rx_fir_config_dec2;
                enable     = 1;
                break;
            case BLADERF_RFIC_RXFIR_DEC4:
                fir_config = &bladerf2_rfic_rx_fir_config_dec4;
                enable     = 1;
                break;
            default:
                RFIC_ERR("invalid RX FIR setting", -EINVAL);
                return false;
        }

        state->filter_invalid[BLADERF_RX] = true;
        {
            int32_t cleanup_status = 0;
            int32_t status = ad9361_set_rx_fir_config_with_cleanup(
                state->phy, *fir_config, &cleanup_status);

            rfic_command_record_cleanup_error(cleanup_status);
            status = rfic_operation_status_with_cleanup(
                status, cleanup_status);
            if (status < 0) {
                RFIC_ERR("AD9361 RX FIR configuration", status);
                return false;
            }
        }
        state->fir_enable_init_rx_filter = rxfir;
        if (init_pair) {
            if (state->init_module_index + 1 < NUM_MODULES) {
                state->filter_invalid[BLADERF_RX] = true;
                return true;
            }
            return _rfic_start_init_fir_pair(state, channel, filter);
        }
        {
            int32_t status = ad9361_fir_enable_transaction_start(
                state->phy, false, enable, &state->job_context.fir_enable_job);

            if (status < 0) {
                rfic_command_record_cleanup_error(
                    state->job_context.fir_enable_job.cleanup_status);
                RFIC_ERR("AD9361 RX FIR enable start", status);
                return false;
            }
        }
        state->fir_enable_job_pending = true;
        state->fir_enable_channel = channel;
        state->fir_enable_filter = filter;
        state->filter_invalid[BLADERF_RX] = true;
        return true;
    }

    RFIC_ERR("invalid RFIC filter channel", -EINVAL);
    return false;
}

bool _rfic_cmd_wr_filter(struct rfic_state *state,
                         bladerf_channel channel,
                         uint32_t filter)
{
    return _rfic_cmd_wr_filter_internal(state, channel, filter, false);
}

static bool _rfic_cmd_wr_filter_init_pair(struct rfic_state *state,
                                          bladerf_channel channel,
                                          uint32_t filter)
{
    return _rfic_cmd_wr_filter_internal(state, channel, filter, true);
}

/**
 * @brief       Get current FIR filter setting
 *
 * @param[in]   channel  The channel
 * @param[out]  filter   The filter state
 *
 * @return      true if successful, false if not
 */
bool _rfic_cmd_rd_filter(struct rfic_state *state,
                         bladerf_channel channel,
                         uint32_t *filter)
{
    if (BLADERF_CHANNEL_IS_TX(channel)) {
        if (state->filter_invalid[BLADERF_TX]) {
            RFIC_ERR("TX FIR cache is invalid", -EIO);
            return false;
        }
        *filter = (uint32_t)state->txfir;
    } else {
        if (state->filter_invalid[BLADERF_RX]) {
            RFIC_ERR("RX FIR cache is invalid", -EIO);
            return false;
        }
        *filter = (uint32_t)state->rxfir;
    }

    return true;
}

/**
 * @brief       Set/unset TX mute
 *
 * @param[in]   channel  The channel
 * @param[in]   mute     Requested mute status
 *
 * @return      true if successful, false if not
 */
bool _rfic_cmd_wr_txmute(struct rfic_state *state,
                         bladerf_channel channel,
                         uint32_t mute)
{
    bool val = (mute > 0);

    CHECK_BOOL(txmute_set(state->phy, channel, val));

    return true;
}

/**
 * @brief       Get current TX mute status
 *
 * @param[in]   channel  The channel
 * @param[out]  mute     Current mute status
 *
 * @return      true if successful, false if not
 */
bool _rfic_cmd_rd_txmute(struct rfic_state *state,
                         bladerf_channel channel,
                         uint32_t *mute)
{
    bool val;

    CHECK_BOOL(txmute_get(state->phy, channel, &val));

    *mute = val ? 1 : 0;

    return true;
}

/**
 * @brief       Store current fastlock parameters into profile
 *
 * @param[in]   channel  The channel
 * @param[in]   profile  The profile number
 *
 * @return      true if successful, false if not
 */
bool _rfic_cmd_wr_fastlock(struct rfic_state *state,
                           bladerf_channel channel,
                           uint32_t profile)
{
    if (BLADERF_CHANNEL_IS_TX(channel)) {
        CHECK_BOOL(ad9361_tx_fastlock_store(state->phy, profile));
    } else {
        CHECK_BOOL(ad9361_rx_fastlock_store(state->phy, profile));
    }

    return true;
}

#endif  // defined(BOARD_BLADERF_MICRO) && defined(BLADERF_NIOS_LIBAD936X)
