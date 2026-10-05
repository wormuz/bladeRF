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
#include "backend/usb/nios_access.h"
#include "board/board.h"
#include "bladerf2_common.h"
#include "common.h"
#include "log.h"
#include "nios_pkt_8x32.h"

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
                        int32_t error_code,
                        uint32_t epoch_id)
{
    struct bladerf_rf_event *evt = &board_data->rf_transition_last_event;

    evt->host_monotonic_ns  = _monotonic_ns();
    evt->fpga_timestamp     = 0; /* Phase 1: FPGA sample-counter correlation
                                  * is out of scope; host timestamps are
                                  * the authoritative trace in this phase. */
    evt->transaction_id     = board_data->rf_transition_current_id;
    /* 0 unless the FPGA data-plane epoch gate actually reported one
     * (ADR-0207 §6, BLADERF_RF_EVT_RX_EPOCH_VALID) -- every other event
     * type passes 0 explicitly, which is honest: there is no epoch to
     * report before the gate confirms one opened. */
    evt->epoch_id           = epoch_id;
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
                   request->target_frequency_hz, 0, 0, 0, 0);
    });

    /* ADR-0207 §6: arm the FPGA data-plane epoch gate BEFORE the retune so
     * pre-retune samples already in flight through the FIFO writer are
     * suppressed from the moment the SPI write lands, not from whenever
     * the host gets around to telling the fabric. epoch_id is the low 8
     * bits of the transaction id -- wraps every 256 transactions, which is
     * fine: it is a snapshot-pairing token for RX_EPOCH_STATUS, not a
     * globally unique identifier.
     *
     * Only armed when the caller actually requires epoch confirmation --
     * BLADERF_RF_REQUIRE_EPOCH_VALID unset means the caller only wants
     * control-plane (PLL+ENSM) confirmation, same opt-in discipline as
     * PLL_LOCKED/ENSM_RX below. Failure here is non-fatal to the retune
     * itself (the control-plane path is unaffected), but it means
     * RX_EPOCH_VALID can never be confirmed for this transaction, so the
     * requirement is downgraded to a logged warning rather than aborting a
     * retune the hardware can still complete correctly on the control
     * plane. */
    if (request->required_events_mask & BLADERF_RF_REQUIRE_EPOCH_VALID) {
        int epoch_status = nios_rx_epoch_ctrl_cmd(dev,
                           NIOS_PKT_8x32_RX_EPOCH_CMD_ARM,
                           (uint8_t)(board_data->rf_transition_current_id & 0xFFu));
        if (epoch_status == 0) {
            /* Settle count must land before COMPLETE in
             * bladerf_rx_transition_wait() -- the gate reads it only at
             * the PENDING -> SETTLING transition, not continuously. */
            epoch_status = nios_rx_epoch_settle_write(dev,
                                                      request->epoch_settle_samples);
        }
        if (epoch_status != 0) {
            log_warning("%s: failed to arm RX epoch gate (status=%d); "
                       "RX_EPOCH_VALID cannot be confirmed for transaction "
                       "%u\n", __FUNCTION__, epoch_status,
                       board_data->rf_transition_current_id);
        }
    }

    _emit_event(board_data, BLADERF_RF_EVT_CONFIG_ACCEPTED,
               BLADERF_RF_STATE_SPI_PROGRAMMING,
               request->target_frequency_hz, 0, 0, 0, 0);

    /* Reuses the existing, already-correct host-mode retune path
     * (bladerf2.c -> rfic_host.c::_rfic_host_set_frequency). This API
     * does NOT reimplement or bypass that logic -- it wraps it with
     * observability, per ADR-0207 "Phase 1 must not change the retune
     * mechanism, only expose its state transitions." */
    status = bladerf_set_frequency(dev, ch, request->target_frequency_hz);
    if (status != 0) {
        _emit_event(board_data, BLADERF_RF_EVT_ERROR, BLADERF_RF_STATE_ERROR,
                   request->target_frequency_hz, 0, 0, status, 0);
        return status;
    }

    /* Real readback, not an assumption that the write succeeded silently
     * (ADR-0207 event contract requires readback_rx_lo_hz to be the
     * observed value). */
    status = bladerf_get_frequency(dev, ch, &readback_hz);
    if (status != 0) {
        _emit_event(board_data, BLADERF_RF_EVT_ERROR, BLADERF_RF_STATE_ERROR,
                   request->target_frequency_hz, 0, 0, status, 0);
        return status;
    }

    _emit_event(board_data, BLADERF_RF_EVT_SPI_DONE,
               BLADERF_RF_STATE_PLL_ACQUIRING,
               request->target_frequency_hz, readback_hz, 0, 0, 0);

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
                           0, 0, 0, status, 0);
                return status;
            }
            if (pll_reg & VCO_LOCK_BIT) {
                pll_locked = true;
                _emit_event(board_data, BLADERF_RF_EVT_RX_PLL_LOCKED,
                           BLADERF_RF_STATE_PLL_LOCKED, 0, 0, pll_reg, 0, 0);
                break;
            }
            usleep(POLL_INTERVAL_US);
        }

        if (!pll_locked) {
            _emit_event(board_data, BLADERF_RF_EVT_ERROR, BLADERF_RF_STATE_ERROR,
                       0, 0, pll_reg, BLADERF_ERR_TIMEOUT, 0);
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
                           0, 0, 0, status, 0);
                return status;
            }
            uint8_t ensm_state = ensm_reg & ENSM_STATE_MASK;
            if (ensm_state == ENSM_STATE_RX || ensm_state == ENSM_STATE_FDD) {
                ensm_rx = true;
                _emit_event(board_data, BLADERF_RF_EVT_ENSM_RX,
                           BLADERF_RF_STATE_RX_PATH_ARMING, 0, 0, ensm_reg, 0, 0);
                break;
            }
            usleep(POLL_INTERVAL_US);
        }

        if (!ensm_rx) {
            _emit_event(board_data, BLADERF_RF_EVT_ERROR, BLADERF_RF_STATE_ERROR,
                       0, 0, ensm_reg, BLADERF_ERR_TIMEOUT, 0);
            if (final_event != NULL) {
                *final_event = board_data->rf_transition_last_event;
            }
            return BLADERF_ERR_TIMEOUT;
        }
    }

    /* ADR-0207 §6: the FPGA data-plane epoch gate is now wired up --
     * RX_EPOCH_VALID means the fabric itself observed real ADC samples
     * past the settle count, not just that PLL/ENSM reported completion.
     * COMPLETE tells the gate the control-plane transition landed (it was
     * armed with ARM in bladerf_rx_transition_begin, before the retune);
     * the gate then counts epoch_settle_samples real samples past that
     * point before opening admission and reporting ACTIVE_NEW here.
     *
     * Only polled if the caller actually requires it, same opt-in
     * discipline as PLL_LOCKED/ENSM_RX above -- a caller that only wants
     * control-plane confirmation does not pay for this poll, and gets
     * exactly the control-plane-only guarantee it asked for. */
    if (board_data->rf_transition_required_events_mask & BLADERF_RF_REQUIRE_EPOCH_VALID) {
        uint32_t epoch_status_word = 0;
        bool epoch_opened = false;

        status = nios_rx_epoch_ctrl_cmd(dev, NIOS_PKT_8x32_RX_EPOCH_CMD_COMPLETE, 0);
        if (status != 0) {
            _emit_event(board_data, BLADERF_RF_EVT_ERROR, BLADERF_RF_STATE_ERROR,
                       0, 0, 0, status, 0);
            return status;
        }

        while (_monotonic_ns() < deadline_ns) {
            status = nios_rx_epoch_status_read(dev, &epoch_status_word);
            if (status != 0) {
                _emit_event(board_data, BLADERF_RF_EVT_ERROR, BLADERF_RF_STATE_ERROR,
                           0, 0, 0, status, 0);
                return status;
            }

            uint8_t epoch_state = (uint8_t)((epoch_status_word >> NIOS_PKT_8x32_RX_EPOCH_STATUS_STATE_SHIFT)
                                            & NIOS_PKT_8x32_RX_EPOCH_STATUS_STATE_MASK);
            if (epoch_state == NIOS_PKT_8x32_RX_EPOCH_STATE_ACTIVE_NEW || epoch_state == NIOS_PKT_8x32_RX_EPOCH_STATE_ACTIVE) {
                epoch_opened = true;
                break;
            }
            if (epoch_state == NIOS_PKT_8x32_RX_EPOCH_STATE_ERROR) {
                _emit_event(board_data, BLADERF_RF_EVT_ERROR, BLADERF_RF_STATE_ERROR,
                           0, 0, epoch_status_word, BLADERF_ERR_UNEXPECTED, 0);
                if (final_event != NULL) {
                    *final_event = board_data->rf_transition_last_event;
                }
                return BLADERF_ERR_UNEXPECTED;
            }
            usleep(POLL_INTERVAL_US);
        }

        if (!epoch_opened) {
            _emit_event(board_data, BLADERF_RF_EVT_ERROR, BLADERF_RF_STATE_ERROR,
                       0, 0, epoch_status_word, BLADERF_ERR_TIMEOUT, 0);
            if (final_event != NULL) {
                *final_event = board_data->rf_transition_last_event;
            }
            /* Failure detection only -- never report this as valid data. */
            return BLADERF_ERR_TIMEOUT;
        }

        _emit_event(board_data, BLADERF_RF_EVT_RX_EPOCH_VALID,
                   BLADERF_RF_STATE_RX_DATA_VALID, 0, 0, epoch_status_word, 0,
                   (epoch_status_word >> NIOS_PKT_8x32_RX_EPOCH_STATUS_EPOCH_ID_SHIFT)
                   & NIOS_PKT_8x32_RX_EPOCH_STATUS_EPOCH_ID_MASK);
    } else {
        _emit_event(board_data, BLADERF_RF_EVT_RX_DATAPATH_ARMED,
                   BLADERF_RF_STATE_RX_DATA_VALID, 0, 0, 0, 0, 0);
    }

    board_data->rf_transition_pending = false;

    if (final_event != NULL) {
        *final_event = board_data->rf_transition_last_event;
    }

    return 0;
}
