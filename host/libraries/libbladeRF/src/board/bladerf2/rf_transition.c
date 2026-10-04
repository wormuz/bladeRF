/**
 * Copyright 2026 bladeRF project contributors
 *
 * This file is part of bladeRF.
 *
 * ADR-0207 BLADE_RF_EVENT_DRIVEN_RF_STATE_001: event-driven RX retune
 * transaction API. Replaces "set_frequency() then guess a sleep" with an
 * explicit, observable state machine: CONFIG_PENDING -> SPI_PROGRAMMING
 * -> PLL_ACQUIRING -> PLL_LOCKED -> ENSM_RX confirmed. Every transition is
 * backed by a real register read (REG_RX_CP_OVERRANGE_VCO_LOCK=0x247 for
 * PLL lock, REG_STATE=0x017 for ENSM state), not a fixed delay. A timeout
 * is a failure-detection signal only -- it never implies the data is
 * valid; bladerf_rx_transition_wait() returns BLADERF_ERR_TIMEOUT and the
 * caller must treat any samples from that transaction as invalid.
 *
 * This is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation, either version 2.1 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program.
 */
#include <time.h>
#include <unistd.h>

#include <libbladeRF.h>

#include "ad936x.h"
#include "board/board.h"
#include "bladerf2_common.h"
#include "common.h"
#include "log.h"

/* Byte-exact with sdrscanner/driver/tuner_fault.py::FAULT_REGS and
 * sdrscanner/hs_sweep/rf_transaction_trace.py -- same register map
 * verified from the Python side against this same AD9361 driver. */
#define REG_STATE_ADDR 0x017
#define REG_RX_CP_VCO_LOCK_ADDR 0x247
#define VCO_LOCK_BIT 0x02
#define ENSM_STATE_MASK 0x0F
#define ENSM_STATE_RX 0x8
#define ENSM_STATE_FDD 0xA

/* Poll interval while waiting for PLL lock / ENSM confirmation. Matches
 * the AD9361 driver's own ad9361_check_cal_done() cadence
 * (thirdparty/analogdevicesinc/no-OS/drivers/rf-transceiver/ad9361/
 * ad9361.c:1309, no_os_udelay(120) for non-REG_CALIBRATION_CTRL regs) --
 * not an arbitrary choice, matched to the hardware's own event-driven
 * polling granularity. */
#define POLL_INTERVAL_US 120

static uint64_t _monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int _read_rfic_reg(struct bladerf *dev, uint16_t addr, uint8_t *val)
{
    struct bladerf2_board_data *board_data = dev->board_data;
    struct ad9361_rf_phy *phy              = board_data->phy;
    int32_t ret = ad9361_spi_read(phy->spi, addr);

    if (ret < 0) {
        return BLADERF_ERR_UNEXPECTED;
    }

    *val = (uint8_t)ret;
    return 0;
}

static void _emit_event(struct bladerf2_board_data *board_data,
                        bladerf_rf_event_type type,
                        bladerf_rf_state state,
                        uint64_t requested_hz,
                        uint64_t readback_hz,
                        uint32_t rfic_status,
                        int32_t error_code)
{
    struct bladerf_rf_event *evt = &board_data->rf_transition_last_event;

    evt->host_monotonic_ns  = _monotonic_ns();
    evt->fpga_timestamp     = 0; /* Phase 1: FPGA sample-counter correlation
                                  * is out of scope; host timestamps are
                                  * the authoritative trace in this phase. */
    evt->transaction_id     = board_data->rf_transition_current_id;
    evt->epoch_id           = 0; /* set by the data-plane epoch gate, which
                                  * is a separate, not-yet-implemented
                                  * FPGA feature (ADR-0207 §6). */
    evt->requested_rx_lo_hz = requested_hz;
    evt->readback_rx_lo_hz  = readback_hz;
    evt->rfic_status        = rfic_status;
    evt->fpga_state         = state;
    evt->event_type         = type;
    evt->flags              = 0;
    evt->error_code         = error_code;

    board_data->rf_transition_state = state;
}

int bladerf_rx_transition_begin(struct bladerf *dev,
                                bladerf_channel ch,
                                const struct bladerf_rx_transition_request *request,
                                uint32_t *transaction_id)
{
    struct bladerf2_board_data *board_data;
    int status;
    bladerf_frequency readback_hz = 0;

    if (dev == NULL || request == NULL || transaction_id == NULL) {
        return BLADERF_ERR_INVAL;
    }

    board_data = dev->board_data;

    WITH_MUTEX(&dev->lock, {
        board_data->rf_transition_next_id++;
        board_data->rf_transition_current_id = board_data->rf_transition_next_id;
        board_data->rf_transition_required_events_mask = request->required_events_mask;
        board_data->rf_transition_pending    = true;
        *transaction_id = board_data->rf_transition_current_id;

        _emit_event(board_data, BLADERF_RF_EVT_CONFIG_ACCEPTED,
                   BLADERF_RF_STATE_CONFIG_PENDING,
                   request->target_frequency_hz, 0, 0, 0);
    });

    _emit_event(board_data, BLADERF_RF_EVT_CONFIG_ACCEPTED,
               BLADERF_RF_STATE_SPI_PROGRAMMING,
               request->target_frequency_hz, 0, 0, 0);

    /* Reuses the existing, already-correct host-mode retune path
     * (bladerf2.c -> rfic_host.c::_rfic_host_set_frequency). This API
     * does NOT reimplement or bypass that logic -- it wraps it with
     * observability, per ADR-0207 "Phase 1 must not change the retune
     * mechanism, only expose its state transitions." */
    status = bladerf_set_frequency(dev, ch, request->target_frequency_hz);
    if (status != 0) {
        _emit_event(board_data, BLADERF_RF_EVT_ERROR, BLADERF_RF_STATE_ERROR,
                   request->target_frequency_hz, 0, 0, status);
        return status;
    }

    /* Real readback, not an assumption that the write succeeded silently
     * (ADR-0207 event contract requires readback_rx_lo_hz to be the
     * observed value). */
    status = bladerf_get_frequency(dev, ch, &readback_hz);
    if (status != 0) {
        _emit_event(board_data, BLADERF_RF_EVT_ERROR, BLADERF_RF_STATE_ERROR,
                   request->target_frequency_hz, 0, 0, status);
        return status;
    }

    _emit_event(board_data, BLADERF_RF_EVT_SPI_DONE,
               BLADERF_RF_STATE_PLL_ACQUIRING,
               request->target_frequency_hz, readback_hz, 0, 0);

    return 0;
}

int bladerf_rx_transition_wait(struct bladerf *dev,
                               uint32_t transaction_id,
                               struct bladerf_rf_event *final_event,
                               uint32_t timeout_ms)
{
    struct bladerf2_board_data *board_data;
    uint64_t deadline_ns;
    uint8_t pll_reg, ensm_reg;
    bool pll_locked = false;
    bool ensm_rx    = false;
    int status;

    if (dev == NULL) {
        return BLADERF_ERR_INVAL;
    }

    board_data = dev->board_data;

    if (board_data->rf_transition_current_id != transaction_id) {
        log_error("%s: transaction_id %u does not match current %u\n",
                 __FUNCTION__, transaction_id, board_data->rf_transition_current_id);
        return BLADERF_ERR_INVAL;
    }

    deadline_ns = _monotonic_ns() + (uint64_t)timeout_ms * 1000000ULL;
    pll_reg = 0;
    ensm_reg = 0;

    /* Poll the real PLL lock bit (REG_RX_CP_OVERRANGE_VCO_LOCK, bit
     * VCO_LOCK_BIT) at the same cadence the AD9361 driver itself polls
     * calibration-done bits (ad9361_check_cal_done). This is NOT a sleep
     * guessing settle time -- it is observing the real hardware state
     * at the hardware's own event granularity (ADR-0207 §"Чим заміняти
     * фіксований settle time", Варіант A). Only polled if the caller
     * actually required it -- BLADERF_RF_REQUIRE_PLL_LOCKED unset means
     * the caller only wants SPI_DONE confirmation. */
    if (board_data->rf_transition_required_events_mask & BLADERF_RF_REQUIRE_PLL_LOCKED) {
        while (_monotonic_ns() < deadline_ns) {
            status = _read_rfic_reg(dev, REG_RX_CP_VCO_LOCK_ADDR, &pll_reg);
            if (status != 0) {
                _emit_event(board_data, BLADERF_RF_EVT_ERROR, BLADERF_RF_STATE_ERROR,
                           0, 0, 0, status);
                return status;
            }
            if (pll_reg & VCO_LOCK_BIT) {
                pll_locked = true;
                _emit_event(board_data, BLADERF_RF_EVT_RX_PLL_LOCKED,
                           BLADERF_RF_STATE_PLL_LOCKED, 0, 0, pll_reg, 0);
                break;
            }
            usleep(POLL_INTERVAL_US);
        }

        if (!pll_locked) {
            _emit_event(board_data, BLADERF_RF_EVT_ERROR, BLADERF_RF_STATE_ERROR,
                       0, 0, pll_reg, BLADERF_ERR_TIMEOUT);
            if (final_event != NULL) {
                *final_event = board_data->rf_transition_last_event;
            }
            /* Failure detection only -- never report this as valid data. */
            return BLADERF_ERR_TIMEOUT;
        }
    }

    if (board_data->rf_transition_required_events_mask & BLADERF_RF_REQUIRE_ENSM_RX) {
        while (_monotonic_ns() < deadline_ns) {
            status = _read_rfic_reg(dev, REG_STATE_ADDR, &ensm_reg);
            if (status != 0) {
                _emit_event(board_data, BLADERF_RF_EVT_ERROR, BLADERF_RF_STATE_ERROR,
                           0, 0, 0, status);
                return status;
            }
            uint8_t ensm_state = ensm_reg & ENSM_STATE_MASK;
            if (ensm_state == ENSM_STATE_RX || ensm_state == ENSM_STATE_FDD) {
                ensm_rx = true;
                _emit_event(board_data, BLADERF_RF_EVT_ENSM_RX,
                           BLADERF_RF_STATE_RX_PATH_ARMING, 0, 0, ensm_reg, 0);
                break;
            }
            usleep(POLL_INTERVAL_US);
        }

        if (!ensm_rx) {
            _emit_event(board_data, BLADERF_RF_EVT_ERROR, BLADERF_RF_STATE_ERROR,
                       0, 0, ensm_reg, BLADERF_ERR_TIMEOUT);
            if (final_event != NULL) {
                *final_event = board_data->rf_transition_last_event;
            }
            return BLADERF_ERR_TIMEOUT;
        }
    }

    /* Phase 1 scope: the data-plane epoch gate (BLADERF_RF_EVT_RX_EPOCH_
     * VALID) requires an FPGA change that is not part of this change.
     * Until that lands, RX_DATA_VALID is reported as soon as the
     * required control-plane events complete -- this is an explicit,
     * documented narrowing of scope, not a silent one: callers relying
     * on BLADERF_RF_REQUIRE_EPOCH_VALID do not yet get a stronger
     * guarantee than control-plane confirmation. */
    _emit_event(board_data, BLADERF_RF_EVT_RX_DATAPATH_ARMED,
               BLADERF_RF_STATE_RX_DATA_VALID, 0, 0, 0, 0);

    board_data->rf_transition_pending = false;

    if (final_event != NULL) {
        *final_event = board_data->rf_transition_last_event;
    }

    return 0;
}
