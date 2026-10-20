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

#include <string.h>

#include <libbladeRF.h>
#include <no_os_delay.h>

#include "ad936x_helpers.h"
#include "bladerf2_common.h"
#include "board/board.h"
#include "common.h"
#include "helpers/wallclock.h"
#include "iterators.h"
#include "log.h"
#include "rfic_fir_policy.h"
#include "rfic_tx_output_policy.h"
#include "rf_transition_policy.h"

// #define BLADERF_HOSTED_C_DEBUG

/* no_os_axi_io has no context parameter; bind it to the device whose RFIC
 * callback is running on this thread. */
extern void bladerf2_axi_io_set_dev(struct bladerf *dev);


/******************************************************************************/
/* Initialization */
/******************************************************************************/

static int _rfic_host_deinitialize(struct bladerf *dev);
static int _rfic_host_deinitialize_internal(struct bladerf *dev,
                                           bool board_reset_asserted);
static int _rfic_host_clear_rffe_control(struct bladerf *dev);

static bool _rfic_host_calibration_faulted(
    struct bladerf2_board_data *board_data)
{
    if (board_data == NULL) {
        return true;
    }
    if (board_data->phy != NULL &&
        ad9361_calibration_state_unknown(board_data->phy)) {
        board_data->calibration_faulted = true;
    }
    if (board_data->calibration_faulted) {
        board_data->rxfir_known = false;
        board_data->txfir_known = false;
    }
    return board_data->calibration_faulted;
}

static void _rfic_host_fail_closed(struct bladerf *dev,
                                   struct bladerf2_board_data *board_data)
{
    int status;
    bool report_tx_fault;

    if (!_rfic_host_calibration_faulted(board_data)) {
        return;
    }

    /* Calibration/partial-mutation poison applies to the shared AD9361, so
     * both RX channels lose validity even when this was reached from TX or a
     * clock/configuration API. Publish RX invalidation before RF cleanup. */
    board_data->rx_rfdc_calibration_valid_mask = 0;
    bladerf2_rx_rfic_state_invalid(dev, BLADERF_ERR_UNEXPECTED);

    /* The RFIC fault invalidates TX even when the failing operation was
     * initiated through RX or a shared-clock API. Revoke async admission
     * before touching hardware so no concurrent producer can pass the stale
     * certificate while RFFE isolation is being attempted. */
    bladerf2_tx_stream_set_faulted(board_data, true);
    MUTEX_LOCK(&board_data->rx_async_epoch_lock);
    report_tx_fault = !board_data->tx_rfic_fault_event_reported;
    board_data->tx_rfic_fault_event_reported = true;
    MUTEX_UNLOCK(&board_data->rx_async_epoch_lock);
    if (report_tx_fault) {
        bladerf2_tx_stream_event(
            dev, BLADERF_RF_EVT_TX_RFIC_STATE_INVALID, 0, 0,
            BLADERF_ERR_UNEXPECTED);
    }

    /* The caller is reporting an RFIC operation whose calibration state is
     * unknown. Disconnect every external RF route immediately; keep the
     * primary ADI error and report RFFE cleanup separately. */
    status = _rfic_host_clear_rffe_control(dev);
    {
        int gate_status = bladerf2_tx_gate_abort_fail_closed(
            dev, board_data, 0);
        if (status < 0 || gate_status < 0) {
            log_error("RFIC fault containment incomplete: RFFE isolation=%s; "
                      "FPGA TX gate abort=%s\n",
                      status < 0 ? bladerf_strerror(status) : "confirmed",
                      gate_status == 0 ? "confirmed" :
                                         bladerf_strerror(gate_status));
        }
    }
}

void bladerf2_rfic_host_fail_closed(struct bladerf *dev)
{
    _rfic_host_fail_closed(dev, dev != NULL ? dev->board_data : NULL);
}

/* Read-only calibration/receive snapshot. Raw register groups are kept
 * separate by channel so one log record can be compared directly with the
 * other. A/C and B/C DC-offset bytes are packed across adjacent registers;
 * retain the raw bytes here rather than implying a decoded engineering unit.
 * This is intentionally debug-only because each sample reads 62 SPI registers. */
static int _rfic_host_metric_read(struct bladerf *dev, uint16_t reg)
{
    uint64_t data = 0;
    uint16_t address = reg | AD936X_READ | AD936X_CNT(1);
    int status = dev->backend->ad9361_spi_read(dev, address, &data);

    return status < 0 ? status : (int)((data >> 56) & 0xff);
}

void bladerf2_rfic_host_log_rx_metrics(struct bladerf *dev,
                                       const char *stage,
                                       uint64_t target_frequency_hz)
{
    static const uint16_t common_regs[] = {
        REG_INPUT_SELECT, REG_STATE, REG_CALIBRATION_CTRL,
        REG_AGC_CONFIG_1, REG_AGC_CONFIG_2, REG_CALIBRATION_CONFIG_1,
        REG_DC_OFFSET_CONFIG2, REG_FORCE_BITS, REG_INVERT_BITS,
        REG_RX_QUAD_CAL_LEVEL, REG_RX_QUAD_GAIN1, REG_RX_QUAD_GAIN2,
        REG_BB_TRACK_CORR_WORD_I_MSB, REG_BB_TRACK_CORR_WORD_I_LSB,
        REG_BB_TRACK_CORR_WORD_Q_MSB, REG_BB_TRACK_CORR_WORD_Q_LSB,
    };
    /* Actual and requested gain state, then per-lane QEC, RF DC, BB DC,
     * RSSI, and overload readbacks. Shared A/BC RF DC bytes are repeated
     * in each lane record to make the two records self-contained. */
    static const uint16_t rx1_regs[] = {
        REG_GAIN_RX1, REG_LPF_GAIN_RX1, REG_DIG_GAIN_RX1,
        REG_RX1_MANUAL_LMT_FULL_GAIN, REG_RX1_MANUAL_LPF_GAIN,
        REG_RX1_MANUAL_DIGITALFORCED_GAIN,
        AD936X_REG_RX1_INPUT_A_PHASE_CORR,
        AD936X_REG_RX1_INPUT_A_GAIN_CORR,
        AD936X_REG_RX1_INPUT_BC_PHASE_CORR,
        AD936X_REG_RX1_INPUT_BC_GAIN_CORR,
        AD936X_REG_RX1_INPUT_A_Q_OFFSET,
        AD936X_REG_RX1_INPUT_A_OFFSETS,
        AD936X_REG_INPUT_A_OFFSETS_1,
        AD936X_REG_RX1_INPUT_BC_Q_OFFSET,
        AD936X_REG_RX1_INPUT_BC_OFFSETS,
        AD936X_REG_INPUT_BC_OFFSETS_1,
        REG_RX1_BB_DC_WORD_I_MSB, REG_RX1_BB_DC_WORD_I_LSB,
        REG_RX1_BB_DC_WORD_Q_MSB, REG_RX1_BB_DC_WORD_Q_LSB,
        REG_RX1_RSSI_SYMBOL, REG_RX1_RSSI_PREAMBLE, REG_OVRG_SIGS_RX1,
    };
    static const uint16_t rx2_regs[] = {
        REG_GAIN_RX2, REG_LPF_GAIN_RX2, REG_DIG_GAIN_RX2,
        REG_RX2_MANUAL_LMT_FULL_GAIN, REG_RX2_MANUAL_LPF_GAIN,
        REG_RX2_MANUAL_DIGITALFORCED_GAIN,
        AD936X_REG_RX2_INPUT_A_PHASE_CORR,
        AD936X_REG_RX2_INPUT_A_GAIN_CORR,
        AD936X_REG_RX2_INPUT_BC_PHASE_CORR,
        AD936X_REG_RX2_INPUT_BC_GAIN_CORR,
        AD936X_REG_RX2_INPUT_A_OFFSETS,
        AD936X_REG_RX2_INPUT_A_I_OFFSET,
        AD936X_REG_INPUT_A_OFFSETS_1,
        AD936X_REG_RX2_INPUT_BC_OFFSETS,
        AD936X_REG_RX2_INPUT_BC_I_OFFSET,
        AD936X_REG_INPUT_BC_OFFSETS_1,
        REG_RX2_BB_DC_WORD_I_MSB, REG_RX2_BB_DC_WORD_I_LSB,
        REG_RX2_BB_DC_WORD_Q_MSB, REG_RX2_BB_DC_WORD_Q_LSB,
        REG_RX2_RSSI_SYMBOL, REG_RX2_RSSI_PREAMBLE, REG_OVRG_SIGS_RX2,
    };
    int common[ARRAY_SIZE(common_regs)];
    int rx1[ARRAY_SIZE(rx1_regs)];
    int rx2[ARRAY_SIZE(rx2_regs)];
    uint32_t rffe = 0;
    size_t i;

    if (dev == NULL || dev->board_data == NULL ||
        ((struct bladerf2_board_data *)dev->board_data)->phy == NULL ||
        log_get_verbosity() > BLADERF_LOG_LEVEL_DEBUG) {
        return;
    }

    for (i = 0; i < ARRAY_SIZE(common_regs); i++) {
        common[i] = _rfic_host_metric_read(dev, common_regs[i]);
    }
    for (i = 0; i < ARRAY_SIZE(rx1_regs); i++) {
        rx1[i] = _rfic_host_metric_read(dev, rx1_regs[i]);
        rx2[i] = _rfic_host_metric_read(dev, rx2_regs[i]);
    }
    if (dev->backend->rffe_control_read != NULL) {
        (void)dev->backend->rffe_control_read(dev, &rffe);
    }

    log_debug("rfic_rx_metrics stage=%s target_hz=%" PRIu64
              " rffe=0x%08" PRIx32
              " input=%d state=%d cal_ctrl=%d agc1=%d agc2=%d"
              " qec_track_cfg=0x%02x dc_track_cfg=0x%02x"
              " force=0x%02x invert=0x%02x qec_level=%d"
              " qec_gain1=%d qec_gain2=%d"
              " bb_track_corr={%d,%d,%d,%d}\n",
              stage != NULL ? stage : "unspecified", target_frequency_hz,
              rffe, common[0], common[1], common[2], common[3], common[4],
              common[5], common[6], common[7], common[8], common[9],
              common[10], common[11], common[12], common[13],
              common[14], common[15]);
    log_debug("rfic_rx_metrics RX1 gain_idx=%d lpf_gain=%d dig_gain=%d"
              " manual_raw={gain:%d,lpf:%d,digital:%d}"
              " manual_idx={gain:%d,lpf:%d,digital:%d}"
              " qec_A={phase:%d,gain:%d} qec_BC={phase:%d,gain:%d}"
              " dc_A={%d,%d,%d} dc_BC={%d,%d,%d}"
              " bb_dc={%d,%d,%d,%d} rssi={symbol:%d,preamble:%d}"
              " overrange=0x%02x\n",
              rx1[0], rx1[1], rx1[2], rx1[3], rx1[4], rx1[5],
              rx1[3] < 0 ? -1 : rx1[3] & RX_FULL_TBL_IDX_MASK,
              rx1[4] < 0 ? -1 : rx1[4] & RX_LPF_IDX_MASK,
              rx1[5] < 0 ? -1 : rx1[5] & RX_DIGITAL_IDX_MASK,
              rx1[6], rx1[7], rx1[8], rx1[9], rx1[10], rx1[11],
              rx1[12], rx1[13], rx1[14], rx1[15], rx1[16], rx1[17],
              rx1[18], rx1[19], rx1[20], rx1[21], rx1[22]);
    log_debug("rfic_rx_metrics RX2 gain_idx=%d lpf_gain=%d dig_gain=%d"
              " manual_raw={gain:%d,lpf:%d,digital:%d}"
              " manual_idx={gain:%d,lpf:%d,digital:%d}"
              " qec_A={phase:%d,gain:%d} qec_BC={phase:%d,gain:%d}"
              " dc_A={%d,%d,%d} dc_BC={%d,%d,%d}"
              " bb_dc={%d,%d,%d,%d} rssi={symbol:%d,preamble:%d}"
              " overrange=0x%02x\n",
              rx2[0], rx2[1], rx2[2], rx2[3], rx2[4], rx2[5],
              rx2[3] < 0 ? -1 : rx2[3] & RX_FULL_TBL_IDX_MASK,
              rx2[4] < 0 ? -1 : rx2[4] & RX_LPF_IDX_MASK,
              rx2[5] < 0 ? -1 : rx2[5] & RX_DIGITAL_IDX_MASK,
              rx2[6], rx2[7], rx2[8], rx2[9], rx2[10], rx2[11],
              rx2[12], rx2[13], rx2[14], rx2[15], rx2[16], rx2[17],
              rx2[18], rx2[19], rx2[20], rx2[21], rx2[22]);
}

/* Every AD9361 access in this controller must first establish that a prior
 * calibration did not leave the chip in an unknown active state. */
#undef CHECK_AD936X
#define CHECK_AD936X(_fn)                                                   \
    do {                                                                    \
        int _cal_state_status = BLADERF_ERR_UNEXPECTED;                     \
        if (_rfic_host_calibration_faulted((dev)->board_data)) {            \
            _rfic_host_fail_closed((dev), (dev)->board_data);                \
            RETURN_ERROR_STATUS("AD9361 calibration state unknown",        \
                                _cal_state_status);                         \
        }                                                                   \
        int _s = (_fn);                                                     \
        if (_s < 0) {                                                       \
            _rfic_host_fail_closed((dev), (dev)->board_data);                \
            RETURN_ERROR_AD9361(#_fn, _s);                                 \
        }                                                                   \
    } while (0)

static bool _rfic_host_is_present(struct bladerf *dev)
{
    return true;
}

static bool _rfic_host_is_initialized(struct bladerf *dev)
{
    if (NULL == dev || NULL == dev->board_data) {
        return false;
    }

    struct bladerf2_board_data *board_data = dev->board_data;

    return (NULL != board_data->phy) &&
           !_rfic_host_calibration_faulted(board_data);
}

static int _rfic_host_get_init_state(struct bladerf *dev,
                                     bladerf_rfic_init_state *state)
{
    bladerf2_axi_io_set_dev(dev);
    if (_rfic_host_is_initialized(dev)) {
        *state = BLADERF_RFIC_INIT_STATE_ON;
    } else {
        *state = BLADERF_RFIC_INIT_STATE_OFF;
    }

    return 0;
}

static bool _rfic_host_is_standby(struct bladerf *dev)
{
    return false;
}

static int _rfic_host_clear_rffe_control(struct bladerf *dev)
{
    uint32_t reg;

    CHECK_STATUS(dev->backend->rffe_control_read(dev, &reg));
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

    return dev->backend->rffe_control_write(dev, reg);
}

int bladerf2_tx_quad_calibration_finalize(struct bladerf *dev);

/* TX QUAD is deferred (manual_tx_quad_cal_en): a rate or bandwidth change
 * leaves it stale. With TX already running, recalibrate now so TX never
 * transmits with the previous configuration's correction; with TX off, the
 * enable path does it. */
static int _rfic_host_txq_refresh(struct bladerf *dev)
{
    uint32_t reg;

    CHECK_STATUS(dev->backend->rffe_control_read(dev, &reg));
    if (!does_rffe_dir_have_enabled_ch(reg, BLADERF_TX)) {
        return 0;
    }
    return bladerf2_tx_quad_calibration_finalize(dev);
}

int bladerf2_tx_quad_calibration_finalize(struct bladerf *dev)
{
    struct bladerf2_board_data *board_data;
    int status, cleanup_status;
    uint32_t reg;

    if (dev == NULL || dev->board_data == NULL || dev->backend == NULL) {
        return BLADERF_ERR_INVAL;
    }
    board_data = dev->board_data;
    if (board_data->phy == NULL) {
        return BLADERF_ERR_UNEXPECTED;
    }

    status = ad9361_ensure_tx_quad_calib(board_data->phy);
    if (status >= 0) {
        return 0;
    }

    if (ad9361_calibration_state_unknown(board_data->phy)) {
        _rfic_host_fail_closed(dev, board_data);
        return errno_ad9361_to_bladerf(status);
    }

    /* A failed calibration after retune must not leave an active TX route.
     * Preserve RX and disconnect both TX SPDTs/lanes at the RFFE boundary. */
    cleanup_status = dev->backend->rffe_control_read(dev, &reg);
    if (cleanup_status == 0) {
        reg = rfic_tx_disconnect_value(
            reg, 1u << RFFE_CONTROL_TXNRX,
            (1u << RFFE_CONTROL_MIMO_TX_EN_0) |
                (1u << RFFE_CONTROL_MIMO_TX_EN_1),
            (RFFE_CONTROL_SPDT_MASK << RFFE_CONTROL_TX_SPDT_1) |
                (RFFE_CONTROL_SPDT_MASK << RFFE_CONTROL_TX_SPDT_2));
        cleanup_status = dev->backend->rffe_control_write(dev, reg);
    }
    if (cleanup_status < 0) {
        log_error("TX calibration failed (%s); TX RFFE disconnect also failed (%s)\n",
                  bladerf_strerror(errno_ad9361_to_bladerf(status)),
                  bladerf_strerror(cleanup_status));
    }
    return errno_ad9361_to_bladerf(status);
}

static int _rfic_host_initialize(struct bladerf *dev)
{
    struct bladerf2_board_data *board_data = dev->board_data;
    struct ad9361_rf_phy *phy              = NULL;
    struct controller_fns const *rfic      = board_data->rfic;
    bladerf_channel ch;
    size_t i;
    uint32_t config_gpio;
    int status;
    int cleanup_status;
    bool calibration_recovery = _rfic_host_calibration_faulted(board_data);

    log_debug("%s: initializating\n", __FUNCTION__);

    board_data->rx_rfdc_calibration_valid_mask = 0;
    memset(board_data->rx_rfdc_calibration_frequency_hz, 0,
           sizeof(board_data->rx_rfdc_calibration_frequency_hz));

    if (calibration_recovery) {
        uint32_t reset_reg;

        /* A timed-out calibration has no documented abort. Assert board
         * RESETB first; only then may the quarantined PHY resources retire. */
        CHECK_STATUS(_rfic_host_clear_rffe_control(dev));
        CHECK_STATUS(dev->backend->rffe_control_read(dev, &reset_reg));
        reset_reg &= ~(1u << RFFE_CONTROL_RESET_N);
        CHECK_STATUS(dev->backend->rffe_control_write(dev, reset_reg));
        no_os_udelay(2);
        status = _rfic_host_deinitialize_internal(dev, true);
        if (status < 0) {
            return status;
        }
        reset_reg |= 1u << RFFE_CONTROL_RESET_N;
        CHECK_STATUS(dev->backend->rffe_control_write(dev, reset_reg));
        /* UG-570 requires at least 1 us after RESETB rises before programming. */
        no_os_mdelay(1);
    }

    /* Reinitialization may replace RFIC state before both filters are
     * reapplied. Do not preserve an enum cache from a prior owner/session. */
    board_data->rxfir_known = false;
    board_data->txfir_known = false;

    /* Keep the RF path disconnected until AD9361 init is complete. */
    CHECK_STATUS(_rfic_host_clear_rffe_control(dev));

    CHECK_STATUS(dev->backend->config_gpio_read(dev, &config_gpio));

    board_data->rfic_init_params = (config_gpio & BLADERF_GPIO_PACKET_CORE_PRESENT) ?
                (void *)&bladerf2_rfic_init_params_fastagc_burst :
                (void *)&bladerf2_rfic_init_params;

    /* Initialize AD9361
     *
     * ad9361_init() used to take the device handle as a third argument and
     * stash it for the SPI and GPIO accessors. It now builds its descriptors
     * from the init parameters, so the handle travels in the .extra field of
     * those parameters instead. The parameter block is shared, so this has to
     * be filled in on every init rather than once at compile time. Keep the
     * per-device values in a local copy: the base parameter blocks are
     * process-global, and mutating their .extra fields races when two radios
     * initialize concurrently. */
    {
        AD9361_InitParam init_params =
            *(AD9361_InitParam *)board_data->rfic_init_params;
        int32_t init_status;
        int32_t init_cleanup_status = 0;
        /* The no_os_axi_io accessors (FPGA AXI AD9361 core over NIOS) have no
         * context argument, so bind this thread to the device for init. */
        bladerf2_axi_io_set_dev(dev);

        init_params.spi_param.extra = dev;
        init_params.gpio_resetb.extra = dev;
        init_params.gpio_sync.extra = dev;
        init_params.gpio_cal_sw1.extra = dev;
        init_params.gpio_cal_sw2.extra = dev;

        init_status = ad9361_init_with_cleanup(
            &phy, &init_params, &init_cleanup_status);
        if (init_status < 0) {
            if (init_cleanup_status < 0) {
                log_error("AD9361 init failed (%s); cleanup also failed (%s)\n",
                          bladerf_strerror(
                              errno_ad9361_to_bladerf(init_status)),
                          bladerf_strerror(
                              errno_ad9361_to_bladerf(init_cleanup_status)));
            }
            /* A quarantined start can make cleanup return ownership of a
             * partial PHY. Retain it so the next INIT can assert RESETB
             * before releasing its resources. */
            board_data->calibration_faulted = true;
            if (phy != NULL)
                board_data->phy = phy;
            bladerf2_axi_io_set_dev(NULL);
            RETURN_ERROR_AD9361("ad9361_init(&phy, &init_params)",
                                init_status);
        }
    }

    if (NULL == phy || NULL == phy->pdata) {
        status = BLADERF_ERR_UNEXPECTED;
        if (NULL != phy) {
            bool consumed = false;
            int remove_status = ad9361_remove_ex(phy, false, &consumed);
            if (!consumed) {
                board_data->phy = phy;
                board_data->calibration_faulted = true;
            }
            if (remove_status < 0) {
                board_data->calibration_faulted = true;
                log_error("Failed to remove invalid AD9361 context: %s\n",
                          bladerf_strerror(
                              errno_ad9361_to_bladerf(remove_status)));
                status = errno_ad9361_to_bladerf(remove_status);
            }
        }
        bladerf2_axi_io_set_dev(NULL);
        RETURN_ERROR_STATUS("ad9361_init struct initialization",
                            status);
    }

    log_verbose("%s: ad9361 initialized @ %p\n", __FUNCTION__, phy);

    board_data->phy = phy;

    /* Run TX QUAD calibration only where TX is about to be used, not after
     * every sample-rate or bandwidth change. Those changes invalidate the
     * stored result; bladerf2_tx_quad_calibration_finalize() reruns it when
     * TX is enabled or retuned, and _rfic_host_txq_refresh() when it
     * already is. Measured 2026-10-20 on xA4 (host RFIC): 61.44 -> 1.625
     * Msps and back took 3.3 s, 2 s of it TX QUAD on an RX-only receiver;
     * deferred, 1.34 s. */
    phy->manual_tx_quad_cal_en = true;

    /* ADI setup completes RFDC after programming this configured input and
     * RX LO. Seed only that exact result; the bladeRF band mapper may select
     * another AD9361 input before the first RX epoch. */
    {
        const AD9361_InitParam *init_param = board_data->rfic_init_params;
        const uint32_t port = init_param->rx_rf_port_input_select;
        if (port < 3 && init_param->rx_synthesizer_frequency_hz != 0) {
            board_data->rx_rfdc_calibration_frequency_hz[port] =
                init_param->rx_synthesizer_frequency_hz;
            board_data->rx_rfdc_calibration_valid_mask |= 1u << port;
        }
        log_debug("%s: RFDC init history selected_input=%" PRIu32
                  " init_lo=%" PRIu64 " valid_mask=0x%02x\n",
                  __FUNCTION__, port,
                  (uint64_t)init_param->rx_synthesizer_frequency_hz,
                  board_data->rx_rfdc_calibration_valid_mask);
    }

    /* ad9361_init() programs both configured operating LOs and completes its
     * setup calibrations there. A temporary reset-frequency hop adds no
     * calibration coverage: RX port selection is deferred while RX is off,
     * and the final LO setter returns to the already configured frequency. */

    /* Set up AD9361 FIR filters */
    /* TODO: permit specification of filter taps, for the truly brave */
    status = rfic->set_filter(dev, BLADERF_CHANNEL_RX(0),
                              BLADERF_RFIC_RXFIR_DEFAULT, 0);
    if (status < 0) {
        goto init_fail;
    }
    status = rfic->set_filter(dev, BLADERF_CHANNEL_TX(0), 0,
                              BLADERF_RFIC_TXFIR_DEFAULT);
    if (status < 0) {
        goto init_fail;
    }

    /* Leave the RF path disconnected until modules are explicitly enabled. */
    status = _rfic_host_clear_rffe_control(dev);
    if (status < 0) {
        goto init_fail;
    }

    /* Move AD9361 back to desired frequency */
    status = rfic->set_frequency(dev, BLADERF_CHANNEL_RX(0),
                                 phy->pdata->rx_synth_freq);
    if (status < 0) {
        goto init_fail;
    }
    status = rfic->set_frequency(dev, BLADERF_CHANNEL_TX(0),
                                 phy->pdata->tx_synth_freq);
    if (status < 0) {
        goto init_fail;
    }

    /* Mute TX channels */
    FOR_EACH_CHANNEL(BLADERF_TX, dev->board->get_channel_count(dev, BLADERF_TX),
                     i, ch)
    {
        status = rfic->set_txmute(dev, ch, true);
        if (status < 0) {
            goto init_fail;
        }
    }

    log_debug("%s: initialization complete\n", __FUNCTION__);

    board_data->calibration_faulted = false;
    MUTEX_LOCK(&board_data->rx_async_epoch_lock);
    board_data->tx_rfic_fault_event_reported = false;
    board_data->rx_rfic_fault_event_reported = false;
    MUTEX_UNLOCK(&board_data->rx_async_epoch_lock);

    bladerf2_rfic_host_log_rx_metrics(
        dev, "host-init-complete", phy->pdata->rx_synth_freq);

    bladerf2_axi_io_set_dev(NULL);
    return 0;

init_fail:
    /* Initialization is transactional from the board's point of view. The
     * RFIC may be partially programmed, so disconnect the RF path, remove
     * the partial ADI context, and make a later INIT start from OFF. Keep the
     * original error unless cleanup fails: that leaves hardware state less
     * certain and must be visible to the caller. */
    board_data->calibration_faulted = true;
    cleanup_status = _rfic_host_clear_rffe_control(dev);
    if (cleanup_status < 0) {
        log_error("Failed to disconnect RFFE after AD9361 init error %d: %s\n",
                  status, bladerf_strerror(cleanup_status));
    }
    if (NULL != board_data->phy) {
        int remove_status;

        if (ad9361_calibration_state_unknown(board_data->phy)) {
            board_data->calibration_faulted = true;
        }
        {
            bool consumed = false;
            remove_status = ad9361_remove_ex(board_data->phy, false,
                                              &consumed);
            if (consumed)
                board_data->phy = NULL;
        }
        if (remove_status < 0) {
            log_error("Failed to remove partial AD9361 context after init "
                      "error %d: %s\n", status,
                      bladerf_strerror(remove_status));
            if (cleanup_status == 0) {
                cleanup_status = remove_status;
            }
        }
    }
    board_data->rxfir_known = false;
    board_data->txfir_known = false;
    bladerf2_axi_io_set_dev(NULL);
    return cleanup_status < 0 ? cleanup_status : status;
}

static int _rfic_host_deinitialize_internal(struct bladerf *dev,
                                            bool board_reset_asserted)
{
    struct bladerf2_board_data *board_data = dev->board_data;
    int status;

    bladerf2_axi_io_set_dev(dev);

    log_debug("%s: deinitializing\n", __FUNCTION__);

    (void)_rfic_host_calibration_faulted(board_data);

    status = _rfic_host_clear_rffe_control(dev);

    if (status >= 0 && board_reset_asserted) {
        uint32_t reset_reg;

        status = dev->backend->rffe_control_read(dev, &reset_reg);
        if (status >= 0 &&
            (reset_reg & (1u << RFFE_CONTROL_RESET_N)) != 0) {
            status = BLADERF_ERR_UNEXPECTED;
        }
    }

    /* Keep the PHY alive if the RF path could not be disconnected. */
    if (status >= 0 && NULL != board_data->phy) {
        struct ad9361_rf_phy *phy = board_data->phy;

        bool consumed = false;

        status = ad9361_remove_ex(phy, board_reset_asserted, &consumed);
        if (consumed)
            board_data->phy = NULL;
    }

    board_data->rxfir_known = false;
    board_data->txfir_known = false;
    bladerf2_axi_io_set_dev(NULL);
    return status;
}

static int _rfic_host_deinitialize(struct bladerf *dev)
{
    struct bladerf2_board_data *board_data = dev->board_data;

    if (_rfic_host_calibration_faulted(board_data)) {
        uint32_t reset_reg;
        int status;

        /* A quarantined calibration has no software abort. Use board RESETB
         * as the hardware retirement barrier before freeing its PHY. */
        status = _rfic_host_clear_rffe_control(dev);
        if (status < 0) {
            return status;
        }
        status = dev->backend->rffe_control_read(dev, &reset_reg);
        if (status < 0) {
            return status;
        }
        reset_reg &= ~(1u << RFFE_CONTROL_RESET_N);
        status = dev->backend->rffe_control_write(dev, reset_reg);
        if (status < 0) {
            return status;
        }
        no_os_udelay(2);

        status = _rfic_host_deinitialize_internal(dev, true);
        if (status < 0) {
            return status;
        }

        reset_reg |= 1u << RFFE_CONTROL_RESET_N;
        status = dev->backend->rffe_control_write(dev, reset_reg);
        if (status == 0) {
            /* Match UG-570's minimum RESETB-to-SPI startup interval. */
            no_os_mdelay(1);
        }
        return status;
    }

    return _rfic_host_deinitialize_internal(dev, false);
}

static int _rfic_host_standby(struct bladerf *dev)
{
    return _rfic_host_deinitialize(dev);
}


/******************************************************************************/
/* Enable */
/******************************************************************************/

static int _rfic_host_enable_module(struct bladerf *dev,
                                    bladerf_channel ch,
                                    bool enable)
{
    bladerf2_axi_io_set_dev(dev);
    struct bladerf2_board_data *board_data = dev->board_data;
    struct ad9361_rf_phy *phy              = board_data->phy;
    struct controller_fns const *rfic      = board_data->rfic;
    bladerf_direction dir = BLADERF_CHANNEL_IS_TX(ch) ? BLADERF_TX : BLADERF_RX;
    bladerf_frequency freq = 0;
    bladerf_channel_layout layout;
    uint32_t reg;       /* RFFE register value */
    uint32_t reg_old;   /* Original RFFE register value */
    int ch_bit;         /* RFFE MIMO channel bit */
    int dir_bit;        /* RFFE RX/TX enable bit */
    bool ch_pending;    /* Requested channel state differs */
    bool dir_enable;    /* Direction is enabled */
    bool dir_pending;   /* Requested direction state differs */
    bool backend_clear; /* Backend requires reset */
    bool mimo_enabled;
    bool const rfic_faulted = _rfic_host_calibration_faulted(board_data);

    if (enable && rfic_faulted) {
        _rfic_host_fail_closed(dev, board_data);
        RETURN_ERROR_STATUS("AD9361 calibration state unknown",
                            BLADERF_ERR_UNEXPECTED);
    }

    /* Look up the RFFE bits affecting this channel */
    ch_bit  = _get_rffe_control_bit_for_ch(ch);
    dir_bit = _get_rffe_control_bit_for_dir(dir);
    if (ch_bit < 0 || dir_bit < 0) {
        RETURN_ERROR_STATUS("_get_rffe_control_bit", BLADERF_ERR_INVAL);
    }

    /* Query the current frequency if necessary */
    if (enable) {
        CHECK_STATUS(rfic->get_frequency(dev, ch, &freq));
    }

    /**
     * If we are moving from 0 active channels to 1 active channel:
     *  Channel:    Setup SPDT, MIMO, TX Mute
     *  Direction:  Setup ENABLE/TXNRX, RFIC port
     *  Backend:    Enable
     *
     * If we are moving from 1 active channel to 0 active channels:
     *  Channel:    Teardown SPDT, MIMO, TX Mute
     *  Direction:  Teardown ENABLE/TXNRX, RFIC port, Sync
     *  Backend:    Disable
     *
     * If we are enabling an nth channel, where n > 1:
     *  Channel:    Setup SPDT, MIMO, TX Mute
     *  Direction:  no change
     *  Backend:    Clear
     *
     * If we are disabling an nth channel, where n > 1:
     *  Channel:    Teardown SPDT, MIMO, TX Mute
     *  Direction:  no change
     *  Backend:    no change
     */

    /* Read RFFE control register */
    CHECK_STATUS(dev->backend->rffe_control_read(dev, &reg));
    reg_old    = reg;
    ch_pending = _rffe_ch_enabled(reg, ch) != enable;
    /* board_data->sync is indexed by direction. Plain sync-> is sync[0],
     * which is always the RX stream, so enabling or disabling a TX
     * channel read the RX layout here and decided the MIMO question from
     * the wrong stream. */
    layout = board_data->sync[dir].stream_config.layout;
    mimo_enabled = layout == BLADERF_RX_X2 || layout == BLADERF_TX_X2;

    if (layout == BLADERF_TX_X2) {
        if (enable) {
            log_debug("Enabling both TX channels. MIMO layout configured.\n");
            reg |= (1 << _get_rffe_control_bit_for_ch(BLADERF_CHANNEL_TX(0)));
            reg |= (1 << _get_rffe_control_bit_for_ch(BLADERF_CHANNEL_TX(1)));
        } else {
            log_debug("Disabling both TX channels. MIMO layout configured.\n");
            reg &= ~(1 << _get_rffe_control_bit_for_ch(BLADERF_CHANNEL_TX(0)));
            reg &= ~(1 << _get_rffe_control_bit_for_ch(BLADERF_CHANNEL_TX(1)));
        }
    }

    if (layout == BLADERF_RX_X2) {
        if (enable) {
            log_debug("Enabling both RX channels. MIMO layout configured.\n");
            reg |= (1 << _get_rffe_control_bit_for_ch(BLADERF_CHANNEL_RX(0)));
            reg |= (1 << _get_rffe_control_bit_for_ch(BLADERF_CHANNEL_RX(1)));
        } else {
            log_debug("Disabling both RX channels. MIMO layout configured.\n");
            reg &= ~(1 << _get_rffe_control_bit_for_ch(BLADERF_CHANNEL_RX(0)));
            reg &= ~(1 << _get_rffe_control_bit_for_ch(BLADERF_CHANNEL_RX(1)));
        }
    }

    /* Channel Setup/Teardown */
    if (ch_pending) {
        /* Modify SPDT bits */
        CHECK_STATUS(_modify_spdt_bits_by_freq(&reg, ch, enable, freq));

        /* Modify MIMO channel enable bits */
        if (enable) {
            reg |= (1 << ch_bit);
        } else {
            reg &= ~(1 << ch_bit);
        }

        /* Set/unset TX mute */
        if (BLADERF_CHANNEL_IS_TX(ch) && !rfic_faulted) {
            /* A failed mute/unmute must stop channel setup. In particular,
             * don't publish the RFFE enable bit or start the FPGA stream if
             * the RFIC did not confirm the requested TX mute state. */
            CHECK_AD936X(txmute_set(phy, ch, !enable));
        }
    }

    dir_enable  = enable || does_rffe_dir_have_enabled_ch(reg, dir);
    dir_pending = _rffe_dir_enabled(reg, dir) != dir_enable;

    /* Direction Setup/Teardown */
    if (dir_pending) {
        /* Modify ENABLE/TXNRX bits */
        if (dir_enable) {
            reg |= (1 << dir_bit);
        } else {
            reg &= ~(1 << dir_bit);
        }

        /* Select RFIC port */
        if (!rfic_faulted) {
            CHECK_AD936X(set_ad9361_port_by_freq(phy, ch, dir_enable, freq));
        }

        /* Tear down sync interface if required */
        if (!dir_enable) {
            sync_deinit(&board_data->sync[dir]);
            perform_format_deconfig(dev, dir);
        }
    }

    /* TX calibration depends on the final RFIC output selector as well as
     * the LO. Recheck after port selection, before RFFE makes TX available. */
    if (enable && BLADERF_CHANNEL_IS_TX(ch)) {
        CHECK_STATUS(bladerf2_tx_quad_calibration_finalize(dev));
    }

    /* Reset FIFO if we are enabling an additional RX channel */
    backend_clear = enable && !dir_pending && BLADERF_RX == dir && !mimo_enabled;

    /* Write RFFE control register */
    if (reg_old != reg) {
        CHECK_STATUS(dev->backend->rffe_control_write(dev, reg));
    } else {
        log_debug("%s: reg value unchanged? (%08x)\n", __FUNCTION__, reg);
    }

    /* Backend Setup/Teardown/Reset */
    if (dir_pending || backend_clear) {
        if (!dir_enable || backend_clear) {
            CHECK_STATUS(dev->backend->enable_module(dev, dir, false));
        }

        if (dir_enable || backend_clear) {
            if (dir == BLADERF_RX) {
                struct bladerf_sync *sync = &board_data->sync[BLADERF_RX];

                if (sync->initialized) {
                    CHECK_STATUS(sync_prime_stream(sync, 0));
                }
            }

            CHECK_STATUS(dev->backend->enable_module(dev, dir, true));
        }
    }

    /* Warn the user if the sample rate isn't reasonable */
    if (ch_pending && enable) {
        check_total_sample_rate(dev);
    }

#ifdef BLADERF_HOSTED_C_DEBUG
    /* Debug logging */
#if defined(_MSC_VER)
    static __declspec(thread) uint64_t lastrun = 0;
#else
    static __thread uint64_t lastrun = 0;
#endif
    /* Keep this diagnostic per thread: two device handles can enable modules
     * concurrently, and a process-wide function-static timestamp is a data
     * race even though it is used only for logging. */
    uint64_t nsec           = wallclock_get_current_nsec();

    log_debug("%s: %s%d ch_en=%d ch_pend=%d dir_en=%d dir_pend=%d be_clr=%d "
              "reg=0x%08x->0x%08x nsec=%" PRIu64 " (delta: %" PRIu64 ")\n",
              __FUNCTION__, BLADERF_TX == dir ? "TX" : "RX", (ch >> 1) + 1,
              enable, ch_pending, dir_enable, dir_pending, backend_clear,
              reg_old, reg, nsec, nsec - lastrun);

    lastrun = nsec;
#endif

    /** Force rerun frequency calibration */
    if (enable) {
        bladerf_frequency current_frequency;
        CHECK_STATUS(dev->board->get_frequency(dev, ch, &current_frequency));
        CHECK_STATUS(dev->board->set_frequency(dev, ch, current_frequency));
    }

    return 0;
}


/******************************************************************************/
/* Sample rate */
/******************************************************************************/

static int _rfic_host_get_sample_rate(struct bladerf *dev,
                                      bladerf_channel ch,
                                      bladerf_sample_rate *rate)
{
    bladerf2_axi_io_set_dev(dev);
    struct bladerf2_board_data *board_data = dev->board_data;
    struct ad9361_rf_phy *phy              = board_data->phy;

    if (BLADERF_CHANNEL_IS_TX(ch)) {
        CHECK_AD936X(ad9361_get_tx_sampling_freq(phy, rate));
    } else {
        CHECK_AD936X(ad9361_get_rx_sampling_freq(phy, rate));
    }

    return 0;
}

static int _rfic_host_set_sample_rate(struct bladerf *dev,
                                      bladerf_channel ch,
                                      bladerf_sample_rate rate)
{
    bladerf2_axi_io_set_dev(dev);
    struct bladerf2_board_data *board_data = dev->board_data;
    struct ad9361_rf_phy *phy              = board_data->phy;
    int32_t status, cleanup_status = 0;

    if (_rfic_host_calibration_faulted(board_data)) {
        _rfic_host_fail_closed(dev, board_data);
        RETURN_ERROR_STATUS("AD9361 calibration state unknown",
                            BLADERF_ERR_UNEXPECTED);
    }

    if (BLADERF_CHANNEL_IS_TX(ch)) {
        status = ad9361_set_tx_sampling_freq_with_cleanup(
            phy, rate, &cleanup_status);
    } else {
        status = ad9361_set_rx_sampling_freq_with_cleanup(
            phy, rate, &cleanup_status);
    }
    if (status < 0 || cleanup_status < 0) {
        _rfic_host_fail_closed(dev, board_data);
        if (cleanup_status < 0) {
            if (status < 0) {
                log_error("AD9361 sample-rate operation returned %s; cleanup "
                          "also returned %s\n",
                          bladerf_strerror(errno_ad9361_to_bladerf(status)),
                          bladerf_strerror(
                              errno_ad9361_to_bladerf(cleanup_status)));
            } else {
                log_error("AD9361 sample-rate operation cleanup failed (%s)\n",
                          bladerf_strerror(
                              errno_ad9361_to_bladerf(cleanup_status)));
            }
            if (status >= 0)
                status = cleanup_status;
        }
        RETURN_ERROR_AD9361("AD9361 sample-rate operation", status);
    }

    return _rfic_host_txq_refresh(dev);
}


/******************************************************************************/
/* Frequency */
/******************************************************************************/

static int _rfic_host_get_frequency(struct bladerf *dev,
                                    bladerf_channel ch,
                                    bladerf_frequency *frequency)
{
    bladerf2_axi_io_set_dev(dev);
    struct bladerf2_board_data *board_data = dev->board_data;
    struct ad9361_rf_phy *phy              = board_data->phy;
    bladerf_frequency lo_frequency;

    if (BLADERF_CHANNEL_IS_TX(ch)) {
        CHECK_AD936X(ad9361_get_tx_lo_freq(phy, &lo_frequency));
    } else {
        CHECK_AD936X(ad9361_get_rx_lo_freq(phy, &lo_frequency));
    }

    *frequency = lo_frequency;

    return 0;
}

static int _rfic_host_set_frequency(struct bladerf *dev,
                                    bladerf_channel ch,
                                    bladerf_frequency frequency)
{
    bladerf2_axi_io_set_dev(dev);
    log_debug("%s: tuning %s LO to %llu Hz\n", __FUNCTION__,
              BLADERF_CHANNEL_IS_TX(ch) ? "TX" : "RX",
              (unsigned long long)frequency);
    struct bladerf2_board_data *board_data = dev->board_data;
    struct ad9361_rf_phy *phy              = board_data->phy;
    struct controller_fns const *rfic      = board_data->rfic;
    struct bladerf_range const *range      = NULL;
    int32_t lo_status;

    if (!BLADERF_CHANNEL_IS_TX(ch) && board_data->rf_transition_pending &&
        !board_data->rf_transition_setter_active) {
        return BLADERF_ERR_WOULD_BLOCK;
    }

    MUTEX_LOCK(&board_data->rx_async_epoch_lock);
    uint8_t rx_retune_pending_count = board_data->rx_retune2_pending_count;
    bool rx_retune_outcome_unknown = board_data->rx_retune2_outcome_unknown;
    MUTEX_UNLOCK(&board_data->rx_async_epoch_lock);
    if (bladerf2_rx_retune_blocks_rf_tune(
            rx_retune_pending_count, rx_retune_outcome_unknown)) {
        /* A timestamped RX fastlock may have executed while its completion
         * record is still unread. Do not tune either RFPLL using a stale
         * peer-LO cache during that interval. */
        return BLADERF_ERR_WOULD_BLOCK;
    }

    /* In dependent FDD mode the peer LO selects which VCO table is valid.
     * An ambiguous/out-of-band fastlock result leaves that peer unknown;
     * allow an explicit tune of the unknown side to recover it, but block
     * tuning the opposite synthesizer until then. */
    if (bladerf2_rf_tune_peer_lo_unknown(
            BLADERF_CHANNEL_IS_TX(ch),
            phy->pdata->fdd && !phy->pdata->fdd_independent_mode,
            phy->current_rx_lo_freq != 0,
            phy->current_tx_lo_freq != 0)) {
        return BLADERF_ERR_WOULD_BLOCK;
    }

    CHECK_STATUS(dev->board->get_frequency_range(dev, ch, &range));

    if (!is_within_range(range, frequency)) {
        log_error("Frequency out of range [%0.2f MHz, %0.2f MHz]: %f MHz\n",
            range->min/1e6, range->max/1e6, frequency/1e6);
        return BLADERF_ERR_RANGE;
    }

    /* A quick tune is recalled by the FPGA's Nios core, which writes
     * REG_(RX|TX)_FAST_LOCK_SETUP directly and leaves the part in fastlock
     * mode behind the driver's bookkeeping. Keep fastlock active across
     * quick-to-quick hops; when host tuning takes ownership, exit it here
     * before programming the synthesizer. Without this transition the RFPLL
     * charge pump can saturate low (0x247 reads 0x40), causing repeated lock
     * failures. This call reads first and writes nothing when fastlock is
     * already inactive. */
    CHECK_AD936X(ad9361_fastlock_exit_foreign(phy, BLADERF_CHANNEL_IS_TX(ch)));

    /* Set up band selection */
    CHECK_STATUS(rfic->select_band(dev, ch, frequency));

    /* Change LO frequency */
    if (BLADERF_CHANNEL_IS_TX(ch)) {
        CHECK_AD936X(ad9361_set_tx_lo_freq(phy, frequency));
        /* The LO setter can itself switch the AD9361 TX output on a band
         * crossing. Calibrate only after that final selector is established. */
        CHECK_STATUS(bladerf2_tx_quad_calibration_finalize(dev));
    } else {
        lo_status = ad9361_set_rx_lo_freq(phy, frequency);
        CHECK_AD936X(lo_status);
        bladerf2_rfic_host_log_rx_metrics(dev, "rx-frequency-set",
                                          frequency);
    }

    return 0;
}

static int _rfic_host_select_band(struct bladerf *dev,
                                  bladerf_channel ch,
                                  bladerf_frequency frequency)
{
    bladerf2_axi_io_set_dev(dev);
    struct bladerf2_board_data *board_data = dev->board_data;
    struct ad9361_rf_phy *phy              = board_data->phy;
    bladerf_channel port_ch                = ch;
    uint32_t reg;
    uint32_t reg_orig;
    size_t i;

    if (_rfic_host_calibration_faulted(board_data)) {
        _rfic_host_fail_closed(dev, board_data);
        RETURN_ERROR_STATUS("AD9361 calibration state unknown",
                            BLADERF_ERR_UNEXPECTED);
    }

    /* Read RFFE control register */
    CHECK_STATUS(dev->backend->rffe_control_read(dev, &reg));
    reg_orig = reg;

    /* Modify the SPDT bits. */
    /* We have to do this for all the channels sharing the same LO. */
    for (i = 0; i < 2; ++i) {
        bladerf_channel bch = BLADERF_CHANNEL_IS_TX(ch) ? BLADERF_CHANNEL_TX(i)
                                                        : BLADERF_CHANNEL_RX(i);
        bool enabled = _rffe_ch_enabled(reg, bch);

        CHECK_STATUS(_modify_spdt_bits_by_freq(&reg, bch, enabled, frequency));
        if (enabled) {
            port_ch = bch;
        }
    }

    /* Write RFFE control register only when the SPDT bits actually changed.
     *
     * Retuning to the frequency that is already programmed used to rewrite
     * this register unconditionally. Measured on a TX1 -> 40 dB pad -> RX1
     * loopback at 30.72 MSPS, 2440 MHz, manual gain, cyclic tone: a
     * same-frequency set_frequency() left the receive path in one of two
     * stable level states about 2.9 dB apart, and the state persisted until
     * the next call. Strict A/B in one process, interleaved, 5 reps each,
     * reference tone measured by direct FFT over 6 fresh captures:
     *
     *   nothing                        range 0.23 dB, sigma 0.093
     *   stream stop/start              range 0.29 dB, sigma 0.131
     *   stream cycle + set_frequency   range 3.33 dB, sigma 1.323
     *   stream cycle + LO no-op        range 0.21 dB, sigma 0.085
     *
     * Ruled out by measurement: settling time (0 vs 300 ms after the call:
     * sigma 1.788 vs 1.672), RX module enable cycle alone (sigma 0.182),
     * the RX gain table (a repeated set_gain does not clear it, and
     * ad9361_load_gt() returns early for an unchanged band), the SPI bus
     * itself (a frequency read matches control at sigma 0.248), one side
     * only (RX sigma 1.883 vs TX 2.025 - both), spectral leakage (11 bins
     * instead of 3: range 3.68 vs 3.65) and image aliasing (the mirror sits
     * 20-30 dB below the tone and does not track it). RFIC registers
     * 0x200-0x2FF are identical in both states.
     *
     * Skipping the write when nothing changed is safe: the register is
     * read-modify-write, so an unchanged value carries no information, and
     * the AD9361 port is still selected below. */
    if (reg != reg_orig) {
        CHECK_STATUS(dev->backend->rffe_control_write(dev, reg));
    }

    /* Set AD9361 port */
    /* TX calibration is finalized after the LO write. Select the target TX
     * output even while RFFE is disabled so that finalizer sees the path the
     * next enable will actually use. RX retains its existing disabled-port
     * behavior and calibrates on enable. */
    CHECK_AD936X(set_ad9361_port_by_freq(
        phy, port_ch,
        BLADERF_CHANNEL_IS_TX(ch) || _rffe_ch_enabled(reg, port_ch),
        frequency));

    return 0;
}


/******************************************************************************/
/* Bandwidth */
/******************************************************************************/

static int _rfic_host_get_bandwidth(struct bladerf *dev,
                                    bladerf_channel ch,
                                    bladerf_bandwidth *bandwidth)
{
    bladerf2_axi_io_set_dev(dev);
    struct bladerf2_board_data *board_data = dev->board_data;
    struct ad9361_rf_phy *phy              = board_data->phy;

    if (BLADERF_CHANNEL_IS_TX(ch)) {
        CHECK_AD936X(ad9361_get_tx_rf_bandwidth(phy, bandwidth));
    } else {
        CHECK_AD936X(ad9361_get_rx_rf_bandwidth(phy, bandwidth));
    }

    return 0;
}

static int _rfic_host_set_bandwidth(struct bladerf *dev,
                                    bladerf_channel ch,
                                    bladerf_bandwidth bandwidth,
                                    bladerf_bandwidth *actual)
{
    bladerf2_axi_io_set_dev(dev);
    struct bladerf2_board_data *board_data = dev->board_data;
    struct ad9361_rf_phy *phy              = board_data->phy;
    struct controller_fns const *rfic      = board_data->rfic;
    struct bladerf_range const *range      = NULL;
    int32_t status, cleanup_status = 0;

    CHECK_STATUS(dev->board->get_bandwidth_range(dev, ch, &range));

    bandwidth = (unsigned int)clamp_to_range(range, bandwidth);
    if (_rfic_host_calibration_faulted(board_data)) {
        _rfic_host_fail_closed(dev, board_data);
        RETURN_ERROR_STATUS("AD9361 calibration state unknown",
                            BLADERF_ERR_UNEXPECTED);
    }

    if (BLADERF_CHANNEL_IS_TX(ch)) {
        status = ad9361_set_tx_rf_bandwidth_with_cleanup(
            phy, bandwidth, &cleanup_status);
    } else {
        status = ad9361_set_rx_rf_bandwidth_with_cleanup(
            phy, bandwidth, &cleanup_status);
    }
    if (status < 0 || cleanup_status < 0) {
        _rfic_host_fail_closed(dev, board_data);
        if (cleanup_status < 0) {
            if (status < 0) {
                log_error("AD9361 bandwidth operation returned %s; cleanup "
                          "also returned %s\n",
                          bladerf_strerror(errno_ad9361_to_bladerf(status)),
                          bladerf_strerror(
                              errno_ad9361_to_bladerf(cleanup_status)));
            } else {
                log_error("AD9361 bandwidth operation cleanup failed (%s)\n",
                          bladerf_strerror(
                              errno_ad9361_to_bladerf(cleanup_status)));
            }
            if (status >= 0)
                status = cleanup_status;
        }
        RETURN_ERROR_AD9361("AD9361 bandwidth operation", status);
    }

    CHECK_STATUS(_rfic_host_txq_refresh(dev));

    if (actual != NULL) {
        return rfic->get_bandwidth(dev, ch, actual);
    }

    return 0;
}


/******************************************************************************/
/* Gain mode */
/******************************************************************************/

static int _rfic_host_get_gain_mode(struct bladerf *dev,
                                    bladerf_channel ch,
                                    bladerf_gain_mode *mode)
{
    bladerf2_axi_io_set_dev(dev);
    struct bladerf2_board_data *board_data = dev->board_data;
    struct ad9361_rf_phy *phy              = board_data->phy;
    uint8_t const rfic_ch                  = ch >> 1;
    uint8_t gc_mode;
    bool ok;

    if (BLADERF_CHANNEL_IS_TX(ch)) {
        log_warning("%s: automatic gain control not valid for TX channels\n",
                    __FUNCTION__);
        *mode = BLADERF_GAIN_DEFAULT;
        return 0;
    }

    /* Get the gain */
    CHECK_AD936X(ad9361_get_rx_gain_control_mode(phy, rfic_ch, &gc_mode));

    /* Mode conversion */
    if (mode != NULL) {
        *mode = gainmode_ad9361_to_bladerf(gc_mode, &ok);
        if (!ok) {
            RETURN_INVAL("mode", "is not valid");
        }
    }

    return 0;
}

static int _rfic_host_set_gain_mode(struct bladerf *dev,
                                    bladerf_channel ch,
                                    bladerf_gain_mode mode)
{
    bladerf2_axi_io_set_dev(dev);
    struct bladerf2_board_data *board_data = dev->board_data;
    struct ad9361_rf_phy *phy              = board_data->phy;
    uint8_t const rfic_ch                  = ch >> 1;
    enum rf_gain_ctrl_mode gc_mode;
    bool ok;

    if (BLADERF_CHANNEL_IS_TX(ch)) {
        log_debug("%s: gain-control modes are unsupported on TX channels\n",
                  __FUNCTION__);
        return BLADERF_ERR_UNSUPPORTED;
    }

    /* Channel conversion */
    switch (ch) {
        case BLADERF_CHANNEL_RX(0):
            gc_mode = ((AD9361_InitParam *)board_data->rfic_init_params)->gc_rx1_mode;
            break;

        case BLADERF_CHANNEL_RX(1):
            gc_mode = ((AD9361_InitParam *)board_data->rfic_init_params)->gc_rx2_mode;
            break;

        default:
            log_error("%s: unknown channel index (%d)\n", __FUNCTION__, ch);
            return BLADERF_ERR_UNSUPPORTED;
    }

    /* Mode conversion */
    if (mode != BLADERF_GAIN_DEFAULT) {
        gc_mode = gainmode_bladerf_to_ad9361(mode, &ok);
        if (!ok) {
            RETURN_INVAL("mode", "is not valid");
        }
    }

    /* Set the mode! */
    CHECK_AD936X(ad9361_set_rx_gain_control_mode(phy, rfic_ch, gc_mode));

    return 0;
}


/******************************************************************************/
/* Gain */
/* These functions handle offset */
/******************************************************************************/

static int _rfic_host_get_gain(struct bladerf *dev,
                               bladerf_channel ch,
                               int *gain)
{
    bladerf2_axi_io_set_dev(dev);
    struct bladerf2_board_data *board_data = dev->board_data;
    struct ad9361_rf_phy *phy              = board_data->phy;
    struct controller_fns const *rfic      = board_data->rfic;
    int val;
    float offset;

    if (_rfic_host_calibration_faulted(board_data)) {
        RETURN_ERROR_STATUS("AD9361 calibration state unknown",
                            BLADERF_ERR_UNEXPECTED);
    }

    CHECK_STATUS(get_gain_offset(dev, ch, &offset));

    if (BLADERF_CHANNEL_IS_TX(ch)) {
        bool muted;

        CHECK_AD936X(txmute_get(phy, ch, &muted));

        if (muted) {
            struct bladerf_range const *range = NULL;

            CHECK_STATUS(
                dev->board->get_gain_stage_range(dev, ch, "dsa", &range));

            val = -__unscale_int(range, txmute_get_cached(phy, ch));
        } else {
            CHECK_STATUS(rfic->get_gain_stage(dev, ch, "dsa", &val));
        }
    } else {
        CHECK_STATUS(rfic->get_gain_stage(dev, ch, "full", &val));
    }

    *gain = __round_int(val + offset);

    return 0;
}

static int _rfic_host_set_gain(struct bladerf *dev,
                               bladerf_channel ch,
                               int gain)
{
    bladerf2_axi_io_set_dev(dev);
    struct bladerf2_board_data *board_data = dev->board_data;
    struct ad9361_rf_phy *phy              = board_data->phy;
    struct controller_fns const *rfic      = board_data->rfic;
    int val;
    float offset;

    if (_rfic_host_calibration_faulted(board_data)) {
        _rfic_host_fail_closed(dev, board_data);
        RETURN_ERROR_STATUS("AD9361 calibration state unknown",
                            BLADERF_ERR_UNEXPECTED);
    }

    CHECK_STATUS(get_gain_offset(dev, ch, &offset));

    val = gain - offset;

    if (BLADERF_CHANNEL_IS_TX(ch)) {
        bool muted;

        CHECK_AD936X(txmute_get(phy, ch, &muted));

        if (muted) {
            struct bladerf_range const *range = NULL;

            CHECK_STATUS(
                dev->board->get_gain_stage_range(dev, ch, "dsa", &range));

            CHECK_STATUS(txmute_set_cached(phy, ch, -__scale_int(range, val)));
        } else {
            CHECK_STATUS(rfic->set_gain_stage(dev, ch, "dsa", val));
        }
    } else {
        CHECK_STATUS(rfic->set_gain_stage(dev, ch, "full", val));
    }

    return 0;
}


/******************************************************************************/
/* Gain stage */
/* These functions handle scaling */
/******************************************************************************/

static int _rfic_host_get_gain_stage(struct bladerf *dev,
                                     bladerf_channel ch,
                                     char const *stage,
                                     int *gain)
{
    bladerf2_axi_io_set_dev(dev);
    struct bladerf2_board_data *board_data = dev->board_data;
    struct ad9361_rf_phy *phy              = board_data->phy;
    struct bladerf_range const *range      = NULL;
    uint8_t const rfic_ch                  = ch >> 1;
    int val;

    CHECK_STATUS(dev->board->get_gain_stage_range(dev, ch, stage, &range));

    if (BLADERF_CHANNEL_IS_TX(ch)) {
        if (strcmp(stage, "dsa") == 0) {
            uint32_t atten;

            CHECK_AD936X(ad9361_get_tx_attenuation(phy, rfic_ch, &atten));

            val = -atten;
        } else {
            log_error("%s: gain stage '%s' invalid\n", __FUNCTION__, stage);
            return BLADERF_ERR_INVAL;
        }
    } else {
        struct rf_rx_gain rx_gain;
        CHECK_AD936X(ad9361_get_rx_gain(phy, rfic_ch + 1, &rx_gain));
        log_verbose("%s: ad9361_get_rx_gain ch%u -> gain_db=%d digital=%u "
                    "fgt_lmt_index=%u\n",
                    __FUNCTION__, rfic_ch + 1, rx_gain.gain_db,
                    rx_gain.digital_gain, rx_gain.fgt_lmt_index);

        if (strcmp(stage, "full") == 0) {
            val = rx_gain.gain_db;
        } else if (strcmp(stage, "digital") == 0) {
            val = rx_gain.digital_gain;
        } else {
            log_error("%s: gain stage '%s' invalid\n", __FUNCTION__, stage);
            return BLADERF_ERR_INVAL;
        }
    }

    *gain = __unscale_int(range, val);

    return 0;
}

static int _rfic_host_set_gain_stage(struct bladerf *dev,
                                     bladerf_channel ch,
                                     char const *stage,
                                     int gain)
{
    bladerf2_axi_io_set_dev(dev);
    struct bladerf2_board_data *board_data = dev->board_data;
    struct ad9361_rf_phy *phy              = board_data->phy;
    struct bladerf_range const *range      = NULL;
    uint8_t const rfic_ch                  = ch >> 1;
    int val;

    CHECK_STATUS(dev->board->get_gain_stage_range(dev, ch, stage, &range));

    /* Scale/round/clamp as required */
    if (BLADERF_CHANNEL_IS_TX(ch) && gain < -89) {
        val = -89750;
    } else {
        val = __scale_int(range, clamp_to_range(range, gain));
    }

    if (BLADERF_CHANNEL_IS_TX(ch)) {
        if (strcmp(stage, "dsa") == 0) {
            CHECK_AD936X(ad9361_set_tx_attenuation(phy, rfic_ch, -val));
        } else {
            log_warning("%s: gain stage '%s' invalid\n", __FUNCTION__, stage);
            return BLADERF_ERR_INVAL;
        }
    } else {
        if (strcmp(stage, "full") == 0) {
            CHECK_AD936X(ad9361_set_rx_rf_gain(phy, rfic_ch, val));
        } else {
            log_warning("%s: gain stage '%s' invalid\n", __FUNCTION__, stage);
            return BLADERF_ERR_INVAL;
        }
    }

    return 0;
}


/******************************************************************************/
/* RSSI */
/******************************************************************************/

static int _rfic_host_get_rssi(struct bladerf *dev,
                               bladerf_channel ch,
                               int *pre_rssi,
                               int *sym_rssi)
{
    bladerf2_axi_io_set_dev(dev);
    struct bladerf2_board_data *board_data = dev->board_data;
    struct ad9361_rf_phy *phy              = board_data->phy;
    uint8_t const rfic_ch                  = ch >> 1;

    int pre, sym;

    if (BLADERF_CHANNEL_IS_TX(ch)) {
        uint32_t rssi = 0;

        /* The TX power monitor gates the measurement block behind
         * REG_TX_RSSI. Without it those registers stay at zero and this
         * call reports 0 dBm at every gain setting, which reads as "no
         * output" rather than "not measured". Measured on a bladeRF 2.0
         * micro xA4: 0 dBm at TX gains 0/20/40/60 dB with the monitor
         * off, -68/-68/-44/-44 dBm with it on. */
        CHECK_AD936X(
            ad9361_txmon_enable(phy, (0 == rfic_ch) ? TX_1 : TX_2));

        CHECK_AD936X(ad9361_get_tx_rssi(phy, rfic_ch, &rssi));

        pre = __round_int(rssi / 1000.0);
        sym = __round_int(rssi / 1000.0);
    } else {
        struct rf_rssi rssi;

        CHECK_AD936X(ad9361_get_rx_rssi(phy, rfic_ch, &rssi));

        pre = __round_int(rssi.preamble / (float)rssi.multiplier);
        sym = __round_int(rssi.symbol / (float)rssi.multiplier);
    }

    if (NULL != pre_rssi) {
        *pre_rssi = -pre;
    }

    if (NULL != sym_rssi) {
        *sym_rssi = -sym;
    }

    return 0;
}


/******************************************************************************/
/* Filter */
/******************************************************************************/

static int _rfic_host_get_filter(struct bladerf *dev,
                                 bladerf_channel ch,
                                 bladerf_rfic_rxfir *rxfir,
                                 bladerf_rfic_txfir *txfir)
{
    bladerf2_axi_io_set_dev(dev);
    struct bladerf2_board_data *board_data = dev->board_data;

    if (_rfic_host_calibration_faulted(board_data)) {
        RETURN_ERROR_STATUS("AD9361 calibration state unknown",
                            BLADERF_ERR_UNEXPECTED);
    }

    if (rxfir != NULL && !board_data->rxfir_known) {
        log_error("%s: RX FIR configuration is unknown after an incomplete update\n",
                  __FUNCTION__);
        return bladerf2_rfic_fir_cache_status(board_data->rxfir_known);
    }
    if (txfir != NULL && !board_data->txfir_known) {
        log_error("%s: TX FIR configuration is unknown after an incomplete update\n",
                  __FUNCTION__);
        return bladerf2_rfic_fir_cache_status(board_data->txfir_known);
    }

    if (NULL != rxfir) {
        *rxfir = board_data->rxfir;
    }

    if (NULL != txfir) {
        *txfir = board_data->txfir;
    }

    return 0;
}

static int _rfic_host_set_filter(struct bladerf *dev,
                                 bladerf_channel ch,
                                 bladerf_rfic_rxfir rxfir,
                                 bladerf_rfic_txfir txfir)
{
    bladerf2_axi_io_set_dev(dev);
    struct bladerf2_board_data *board_data = dev->board_data;
    struct ad9361_rf_phy *phy              = board_data->phy;
    int32_t status, cleanup_status = 0, enable_cleanup_status = 0;

    if (_rfic_host_calibration_faulted(board_data)) {
        _rfic_host_fail_closed(dev, board_data);
        RETURN_ERROR_STATUS("AD9361 calibration state unknown",
                            BLADERF_ERR_UNEXPECTED);
    }

    if (BLADERF_CHANNEL_IS_TX(ch)) {
        AD9361_TXFIRConfig *fir_config = NULL;
        uint8_t enable;

        /* Either register programming step may partially change hardware.
         * Keep the cached enum unavailable until both complete. */
        board_data->txfir_known = false;

        if (BLADERF_RFIC_TXFIR_CUSTOM == txfir) {
            log_warning("custom FIR not implemented, assuming default\n");
            txfir = BLADERF_RFIC_TXFIR_DEFAULT;
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
                MUTEX_UNLOCK(&dev->lock);
                assert(!"Bug: unhandled txfir selection");
                return BLADERF_ERR_UNEXPECTED;
        }

        status = ad9361_set_tx_fir_config_with_cleanup(
            phy, *fir_config, &cleanup_status);
        if (status < 0 || cleanup_status < 0) {
            _rfic_host_fail_closed(dev, board_data);
            if (cleanup_status < 0) {
                if (status < 0) {
                    log_error("AD9361 TX FIR configuration returned %s; "
                              "cleanup also returned %s\n",
                              bladerf_strerror(
                                  errno_ad9361_to_bladerf(status)),
                              bladerf_strerror(
                                  errno_ad9361_to_bladerf(cleanup_status)));
                } else {
                    log_error("AD9361 TX FIR cleanup failed (%s)\n",
                              bladerf_strerror(
                                  errno_ad9361_to_bladerf(cleanup_status)));
                }
                if (status >= 0)
                    status = cleanup_status;
            }
            RETURN_ERROR_AD9361("AD9361 TX FIR configuration", status);
        }
        status = ad9361_set_tx_fir_en_dis_with_cleanup(
            phy, enable, &enable_cleanup_status);
        if (status < 0 || enable_cleanup_status < 0) {
            _rfic_host_fail_closed(dev, board_data);
            if (enable_cleanup_status < 0) {
                if (status < 0) {
                    log_error("AD9361 TX FIR enable returned %s; cleanup also "
                              "returned %s\n",
                              bladerf_strerror(
                                  errno_ad9361_to_bladerf(status)),
                              bladerf_strerror(errno_ad9361_to_bladerf(
                                  enable_cleanup_status)));
                } else {
                    log_error("AD9361 TX FIR enable cleanup failed (%s)\n",
                              bladerf_strerror(errno_ad9361_to_bladerf(
                                  enable_cleanup_status)));
                    status = enable_cleanup_status;
                }
            }
            RETURN_ERROR_AD9361("AD9361 TX FIR enable", status);
        }

        board_data->txfir = txfir;
        board_data->txfir_known = true;
    } else {
        AD9361_RXFIRConfig *fir_config = NULL;
        uint8_t enable;

        /* Either register programming step may partially change hardware.
         * Keep the cached enum unavailable until both complete. */
        board_data->rxfir_known = false;

        if (BLADERF_RFIC_RXFIR_CUSTOM == rxfir) {
            log_warning("custom FIR not implemented, assuming default\n");
            rxfir = BLADERF_RFIC_RXFIR_DEFAULT;
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
                MUTEX_UNLOCK(&dev->lock);
                assert(!"Bug: unhandled rxfir selection");
                return BLADERF_ERR_UNEXPECTED;
        }

        cleanup_status = 0;
        status = ad9361_set_rx_fir_config_with_cleanup(
            phy, *fir_config, &cleanup_status);
        if (status < 0 || cleanup_status < 0) {
            _rfic_host_fail_closed(dev, board_data);
            if (cleanup_status < 0) {
                if (status < 0) {
                    log_error("AD9361 RX FIR configuration returned %s; "
                              "cleanup also returned %s\n",
                              bladerf_strerror(
                                  errno_ad9361_to_bladerf(status)),
                              bladerf_strerror(
                                  errno_ad9361_to_bladerf(cleanup_status)));
                } else {
                    log_error("AD9361 RX FIR cleanup failed (%s)\n",
                              bladerf_strerror(
                                  errno_ad9361_to_bladerf(cleanup_status)));
                }
                if (status >= 0)
                    status = cleanup_status;
            }
            RETURN_ERROR_AD9361("AD9361 RX FIR configuration", status);
        }
        status = ad9361_set_rx_fir_en_dis_with_cleanup(
            phy, enable, &enable_cleanup_status);
        if (status < 0 || enable_cleanup_status < 0) {
            _rfic_host_fail_closed(dev, board_data);
            if (enable_cleanup_status < 0) {
                if (status < 0) {
                    log_error("AD9361 RX FIR enable returned %s; cleanup also "
                              "returned %s\n",
                              bladerf_strerror(
                                  errno_ad9361_to_bladerf(status)),
                              bladerf_strerror(errno_ad9361_to_bladerf(
                                  enable_cleanup_status)));
                } else {
                    log_error("AD9361 RX FIR enable cleanup failed (%s)\n",
                              bladerf_strerror(errno_ad9361_to_bladerf(
                                  enable_cleanup_status)));
                    status = enable_cleanup_status;
                }
            }
            RETURN_ERROR_AD9361("AD9361 RX FIR enable", status);
        }

        board_data->rxfir = rxfir;
        board_data->rxfir_known = true;
    }

    return 0;
}


/******************************************************************************/
/* TX Mute */
/******************************************************************************/

static int _rfic_host_get_txmute(struct bladerf *dev,
                                 bladerf_channel ch,
                                 bool *state)
{
    bladerf2_axi_io_set_dev(dev);
    struct bladerf2_board_data *board_data = dev->board_data;
    struct ad9361_rf_phy *phy              = board_data->phy;

    if (BLADERF_CHANNEL_IS_TX(ch)) {
        CHECK_AD936X(txmute_get(phy, ch, state));
        return 0;
    }

    return BLADERF_ERR_UNSUPPORTED;
}

static int _rfic_host_set_txmute(struct bladerf *dev,
                                 bladerf_channel ch,
                                 bool state)
{
    bladerf2_axi_io_set_dev(dev);
    struct bladerf2_board_data *board_data = dev->board_data;
    struct ad9361_rf_phy *phy              = board_data->phy;

    if (BLADERF_CHANNEL_IS_TX(ch)) {
        CHECK_AD936X(txmute_set(phy, ch, state));
        return 0;
    }

    return BLADERF_ERR_UNSUPPORTED;
}


/******************************************************************************/
/* Fastlock */
/******************************************************************************/

static int _rfic_host_store_fastlock_profile(struct bladerf *dev,
                                             bladerf_channel ch,
                                             uint32_t profile)
{
    bladerf2_axi_io_set_dev(dev);
    struct bladerf2_board_data *board_data = dev->board_data;
    struct ad9361_rf_phy *phy              = board_data->phy;

    if (BLADERF_CHANNEL_IS_TX(ch)) {
        CHECK_AD936X(ad9361_tx_fastlock_store(phy, profile));
    } else {
        CHECK_AD936X(ad9361_rx_fastlock_store(phy, profile));
    }

    return 0;
}

static int _rfic_host_save_fastlock_profile(struct bladerf *dev,
                                             bladerf_channel ch,
                                             uint32_t profile,
                                             uint8_t *values)
{
    bladerf2_axi_io_set_dev(dev);
    struct bladerf2_board_data *board_data = dev->board_data;
    struct ad9361_rf_phy *phy              = board_data->phy;

    if (BLADERF_CHANNEL_IS_TX(ch)) {
        CHECK_AD936X(ad9361_tx_fastlock_save(phy, profile, values));
    } else {
        CHECK_AD936X(ad9361_rx_fastlock_save(phy, profile, values));
    }

    return 0;
}


/******************************************************************************/
/* Function pointers */
/******************************************************************************/

struct controller_fns const rfic_host_control = {
    FIELD_INIT(.is_present, _rfic_host_is_present),
    FIELD_INIT(.is_standby, _rfic_host_is_standby),
    FIELD_INIT(.is_initialized, _rfic_host_is_initialized),
    FIELD_INIT(.get_init_state, _rfic_host_get_init_state),

    FIELD_INIT(.initialize, _rfic_host_initialize),
    FIELD_INIT(.standby, _rfic_host_standby),
    FIELD_INIT(.deinitialize, _rfic_host_deinitialize),

    FIELD_INIT(.enable_module, _rfic_host_enable_module),

    FIELD_INIT(.get_sample_rate, _rfic_host_get_sample_rate),
    FIELD_INIT(.set_sample_rate, _rfic_host_set_sample_rate),

    FIELD_INIT(.get_frequency, _rfic_host_get_frequency),
    FIELD_INIT(.set_frequency, _rfic_host_set_frequency),
    FIELD_INIT(.select_band, _rfic_host_select_band),

    FIELD_INIT(.get_bandwidth, _rfic_host_get_bandwidth),
    FIELD_INIT(.set_bandwidth, _rfic_host_set_bandwidth),

    FIELD_INIT(.get_gain_mode, _rfic_host_get_gain_mode),
    FIELD_INIT(.set_gain_mode, _rfic_host_set_gain_mode),

    FIELD_INIT(.get_gain, _rfic_host_get_gain),
    FIELD_INIT(.set_gain, _rfic_host_set_gain),

    FIELD_INIT(.get_gain_stage, _rfic_host_get_gain_stage),
    FIELD_INIT(.set_gain_stage, _rfic_host_set_gain_stage),

    FIELD_INIT(.get_rssi, _rfic_host_get_rssi),

    FIELD_INIT(.get_filter, _rfic_host_get_filter),
    FIELD_INIT(.set_filter, _rfic_host_set_filter),

    FIELD_INIT(.get_txmute, _rfic_host_get_txmute),
    FIELD_INIT(.set_txmute, _rfic_host_set_txmute),

    FIELD_INIT(.store_fastlock_profile, _rfic_host_store_fastlock_profile),
    FIELD_INIT(.save_fastlock_profile, _rfic_host_save_fastlock_profile),

    FIELD_INIT(.command_mode, RFIC_COMMAND_HOST),
};
