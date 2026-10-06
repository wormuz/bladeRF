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
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <inttypes.h>
#include <stdlib.h>

#include <libbladeRF.h>

#include "ad936x.h"
#include "backend/usb/nios_access.h"
#include "board/board.h"
#include "bladerf2_common.h"
#include "common.h"
#include "log.h"
#include "nios_pkt_8x32.h"
#include "rf_transition_policy.h"
#include "streaming/sync.h"

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

/* A successful NIOS PIO write only confirms that the command reached the
 * control plane. Wait until the RX-clock-domain gate reports the matching
 * epoch in PENDING before changing the LO, so the data-plane fence is known
 * to be active before the RFIC starts moving. */
static int _wait_rx_epoch_fenced(struct bladerf *dev, uint8_t expected_epoch,
                                 uint32_t timeout_ms, uint32_t *status_word)
{
    uint64_t deadline_ns = _monotonic_ns() +
                           (uint64_t)timeout_ms * 1000000ULL;
    uint32_t value = 0;

    do {
        int status = nios_rx_epoch_status_read(dev, &value);
        if (status != 0) {
            return status;
        }

        uint8_t state = (uint8_t)((value >>
            NIOS_PKT_8x32_RX_EPOCH_STATUS_STATE_SHIFT) &
            NIOS_PKT_8x32_RX_EPOCH_STATUS_STATE_MASK);
        uint8_t epoch = (uint8_t)((value >>
            NIOS_PKT_8x32_RX_EPOCH_STATUS_EPOCH_ID_SHIFT) &
            NIOS_PKT_8x32_RX_EPOCH_STATUS_EPOCH_ID_MASK);

        if (status_word != NULL) {
            *status_word = value;
        }
        if (state == NIOS_PKT_8x32_RX_EPOCH_STATE_ERROR) {
            return BLADERF_ERR_UNEXPECTED;
        }
        if (state == NIOS_PKT_8x32_RX_EPOCH_STATE_PENDING &&
            epoch == expected_epoch) {
            /* Bit 18 is the FPGA capability/arm acknowledgement for
             * sample-META epoch identity. Fail before LO programming on
             * older images that only provide the timestamp boundary. */
            if ((value & (1u << 18)) == 0) {
                return BLADERF_ERR_UNSUPPORTED;
            }
            return 0;
        }

        usleep(POLL_INTERVAL_US);
    } while (_monotonic_ns() < deadline_ns);

    return BLADERF_ERR_TIMEOUT;
}

/* FPGA epoch IDs are 8-bit state owned by the fabric and survive a host
 * close/reopen while the FPGA remains loaded.  They must not be derived from
 * rf_transition_current_id, whose lifetime is one libbladeRF handle. */
static int _read_rx_epoch_state(struct bladerf *dev, uint32_t *status_word,
                                uint8_t *state, uint8_t *epoch)
{
    uint32_t value;
    int status = nios_rx_epoch_status_read(dev, &value);
    if (status != 0) {
        return status;
    }

    *status_word = value;
    *state = (uint8_t)((value >>
        NIOS_PKT_8x32_RX_EPOCH_STATUS_STATE_SHIFT) &
        NIOS_PKT_8x32_RX_EPOCH_STATUS_STATE_MASK);
    *epoch = (uint8_t)((value >>
        NIOS_PKT_8x32_RX_EPOCH_STATUS_EPOCH_ID_SHIFT) &
        NIOS_PKT_8x32_RX_EPOCH_STATUS_EPOCH_ID_MASK);
    return 0;
}

/* A previous handle may have closed while its FPGA gate was still fenced.
 * Invalidate that incomplete epoch, then allocate the next ID relative to
 * the persistent FPGA epoch rather than the new handle. The gate remains
 * fail-closed in ERROR until the next explicit ARM. */
static int _prepare_rx_epoch_id(struct bladerf *dev, uint32_t timeout_ms,
                                uint8_t *epoch_id, uint32_t *status_word)
{
    uint64_t deadline_ns = _monotonic_ns() +
                           (uint64_t)timeout_ms * 1000000ULL;
    uint8_t state, current_epoch;
    int status = _read_rx_epoch_state(dev, status_word, &state, &current_epoch);
    if (status != 0) {
        return status;
    }

    if (state != NIOS_PKT_8x32_RX_EPOCH_STATE_ACTIVE &&
        state != NIOS_PKT_8x32_RX_EPOCH_STATE_ERROR) {
        status = nios_rx_epoch_ctrl_cmd(
            dev, NIOS_PKT_8x32_RX_EPOCH_CMD_ABORT, 0);
        if (status != 0) {
            return status;
        }

        do {
            status = _read_rx_epoch_state(dev, status_word, &state,
                                          &current_epoch);
            if (status != 0) {
                return status;
            }
            if (state == NIOS_PKT_8x32_RX_EPOCH_STATE_ERROR) {
                break;
            }
            usleep(POLL_INTERVAL_US);
        } while (_monotonic_ns() < deadline_ns);

        if (state != NIOS_PKT_8x32_RX_EPOCH_STATE_ERROR) {
            return BLADERF_ERR_TIMEOUT;
        }
    }

    *epoch_id = (uint8_t)(current_epoch + 1u);
    return 0;
}

static void _abort_transition(struct bladerf *dev,
                              struct bladerf2_board_data *board_data,
                              struct bladerf_rf_event *final_event)
{
    if (board_data->rf_transition_required_events_mask &
        BLADERF_RF_REQUIRE_EPOCH_VALID) {
        (void)nios_rx_epoch_ctrl_cmd(dev,
                    NIOS_PKT_8x32_RX_EPOCH_CMD_ABORT, 0);
    }

    WITH_MUTEX(&dev->lock, {
        board_data->rf_transition_pending = false;
        board_data->rf_transition_waiting = false;
        board_data->rf_transition_setter_active = false;
        if (final_event != NULL) {
            *final_event = board_data->rf_transition_last_event;
        }
    });
}

static int _fail_transition(struct bladerf *dev,
                            struct bladerf2_board_data *board_data,
                            int status,
                            struct bladerf_rf_event *final_event)
{
    _abort_transition(dev, board_data, final_event);
    return status;
}

static void _emit_event_with_timestamp(struct bladerf *dev,
                        struct bladerf2_board_data *board_data,
                        bladerf_rf_event_type type,
                        bladerf_rf_state state,
                        uint64_t requested_hz,
                        uint64_t readback_hz,
                        uint32_t rfic_status,
                        int32_t error_code,
                        uint32_t epoch_id,
                        uint64_t host_monotonic_ns,
                        uint64_t fpga_timestamp,
                        uint32_t flags)
{
    struct bladerf_rf_event event = {0};

    /* Later events (PLL, ENSM, epoch-valid) are emitted by wait(), which
     * does not receive the original request again. Preserve the requested
     * frequency and the readback value known at this point in the
     * transaction; zero readback means verification has not happened yet. */
    event.fpga_timestamp    = fpga_timestamp;
    /* 0 unless the FPGA data-plane epoch gate actually reported one
     * (ADR-0207 §6, BLADERF_RF_EVT_RX_EPOCH_VALID) -- every other event
     * type passes 0 explicitly, which is honest: there is no epoch to
     * report before the gate confirms one opened. */
    event.epoch_id           = epoch_id;
    event.rfic_status        = rfic_status;
    event.fpga_state         = state;
    event.event_type         = type;
    event.flags              = flags;
    event.error_code         = error_code;

    WITH_MUTEX(&dev->lock, {
        event.host_monotonic_ns = host_monotonic_ns != 0 ?
                                  host_monotonic_ns : _monotonic_ns();
        event.transaction_id = board_data->rf_transition_current_id;
        if (requested_hz == 0) {
            requested_hz = board_data->rf_transition_requested_frequency_hz;
        }
        if (readback_hz == 0) {
            readback_hz = board_data->rf_transition_readback_frequency_hz;
        }
        event.requested_rx_lo_hz = requested_hz;
        event.readback_rx_lo_hz = readback_hz;

        board_data->rf_transition_last_event = event;
        board_data->rf_transition_events[board_data->rf_transition_event_head] =
            event;
        board_data->rf_transition_event_head =
            (board_data->rf_transition_event_head + 1) %
            BLADERF2_RF_EVENT_HISTORY_SIZE;
        if (board_data->rf_transition_event_count <
            BLADERF2_RF_EVENT_HISTORY_SIZE) {
            board_data->rf_transition_event_count++;
        }
        board_data->rf_transition_state = state;
    });
}

static void _emit_event(struct bladerf *dev,
                        struct bladerf2_board_data *board_data,
                        bladerf_rf_event_type type,
                        bladerf_rf_state state,
                        uint64_t requested_hz,
                        uint64_t readback_hz,
                        uint32_t rfic_status,
                        int32_t error_code,
                        uint32_t epoch_id)
{
    _emit_event_with_timestamp(dev, board_data, type, state, requested_hz,
                               readback_hz, rfic_status, error_code,
                               epoch_id, 0, 0, 0);
}

/* Called by the AD9361 SPI platform adapter while bladerf_set_frequency()
 * holds dev->lock. Timestamps surround actual backend SPI write calls; no
 * mutex is taken here to avoid recursively acquiring the device lock. */
void bladerf2_rx_transition_spi_observe(struct bladerf *dev, bool begin,
                                        int status)
{
    struct bladerf2_board_data *board_data;
    if (dev == NULL || dev->board_data == NULL) {
        return;
    }

    board_data = dev->board_data;
    if (!board_data->rf_transition_spi_trace_enabled ||
        !board_data->rf_transition_pending) {
        return;
    }

    if (begin) {
        if (board_data->rf_transition_spi_write_count == 0) {
            board_data->rf_transition_spi_first_write_ns = _monotonic_ns();
        }
        board_data->rf_transition_spi_write_count++;
    } else {
        board_data->rf_transition_spi_last_write_ns = _monotonic_ns();
        board_data->rf_transition_spi_last_status = status;
    }
}

#ifdef BLADERF_ENABLE_TEST_SPI_FAULT_INJECTION
/* Deliberately absent from normal builds. Tests set the 1-based ordinal in
 * BLADERF_TEST_SPI_FAIL_RX_TRANSITION_WRITE; exactly one backend write in the
 * next active RX transition is then skipped and reported as -EIO. */
int bladerf2_rx_transition_spi_test_should_fail(struct bladerf *dev)
{
    const char *setting = getenv("BLADERF_TEST_SPI_FAIL_RX_TRANSITION_WRITE");
    struct bladerf2_board_data *board_data;
    char *end = NULL;
    unsigned long fail_ordinal;

    if (dev == NULL || dev->board_data == NULL || setting == NULL ||
        setting[0] == '\0') {
        return 0;
    }

    errno = 0;
    fail_ordinal = strtoul(setting, &end, 10);
    if (errno != 0 || end == setting || *end != '\0' || fail_ordinal == 0 ||
        fail_ordinal > UINT32_MAX) {
        return 0;
    }

    board_data = dev->board_data;
    if (!board_data->rf_transition_pending ||
        !board_data->rf_transition_spi_trace_enabled) {
        return 0;
    }

    if (board_data->rf_transition_test_fault_transaction_id !=
        board_data->rf_transition_current_id) {
        board_data->rf_transition_test_fault_transaction_id =
            board_data->rf_transition_current_id;
        board_data->rf_transition_test_fault_write_ordinal = 0;
        board_data->rf_transition_test_fault_consumed = false;
    }

    if (board_data->rf_transition_test_fault_consumed) {
        return 0;
    }

    board_data->rf_transition_test_fault_write_ordinal++;
    if (board_data->rf_transition_test_fault_write_ordinal == fail_ordinal) {
        board_data->rf_transition_test_fault_consumed = true;
        return 1;
    }

    return 0;
}
#endif

void bladerf2_rx_transition_spi_observe_rollback(struct bladerf *dev,
                                                 uint32_t write_count)
{
    struct bladerf2_board_data *board_data;

    if (dev == NULL || dev->board_data == NULL) {
        return;
    }
    board_data = dev->board_data;
    if (!board_data->rf_transition_spi_trace_enabled ||
        !board_data->rf_transition_pending) {
        return;
    }

    if (write_count >= board_data->rf_transition_spi_write_count) {
        board_data->rf_transition_spi_write_count = 0;
        board_data->rf_transition_spi_first_write_ns = 0;
        board_data->rf_transition_spi_last_write_ns = 0;
    } else {
        board_data->rf_transition_spi_write_count -= write_count;
    }
}

static int _bladerf_rx_transition_begin(
    struct bladerf *dev, bladerf_channel ch,
    const struct bladerf_rx_transition_request *request,
    const struct bladerf_quick_tune *quick_tune,
    uint32_t *transaction_id)
{
    struct bladerf2_board_data *board_data;
    int status;
    uint32_t required_events_mask;
    bladerf_frequency readback_hz = 0;
    bool transition_busy = false;
    uint64_t stage_started_ns;
    uint64_t spi_first_write_ns = 0;
    uint64_t spi_last_write_ns = 0;
    uint32_t spi_write_count = 0;
    int spi_last_status = 0;
    uint64_t nios_begin_ns = 0;
    uint64_t nios_out_done_ns = 0;
    uint64_t nios_response_done_ns = 0;
    int nios_transport_status = 0;
    bool nios_trace_valid = false;

    if (dev == NULL || request == NULL || transaction_id == NULL) {
        return BLADERF_ERR_INVAL;
    }
    if (BLADERF_CHANNEL_IS_TX(ch)) {
        return BLADERF_ERR_INVAL;
    }

    board_data = dev->board_data;
    if (!bladerf2_rf_transition_normalize_requirements(
            request->required_events_mask,
            request->require_rx_data_valid,
            &required_events_mask)) {
        return BLADERF_ERR_INVAL;
    }

    /* Timestamped RX metadata is required to fence USB/sync buffers that
     * were already queued before the FPGA epoch gate opened. Raw sync
     * formats cannot prove which side of the epoch boundary a sample is on.
     * Reject before arming or touching the RFIC. */
    if ((required_events_mask & BLADERF_RF_REQUIRE_EPOCH_VALID) &&
        board_data->sync[BLADERF_RX].initialized) {
        status = sync_rx_epoch_require_metadata(
            &board_data->sync[BLADERF_RX]);
        if (status != 0) {
            return status;
        }
    }

    WITH_MUTEX(&dev->lock, {
        if (board_data->rf_transition_pending) {
            transition_busy = true;
        } else {
            board_data->rf_transition_next_id++;
            board_data->rf_transition_current_id = board_data->rf_transition_next_id;
            board_data->rf_transition_required_events_mask = required_events_mask;
            board_data->rf_transition_requested_frequency_hz =
                request->target_frequency_hz;
            board_data->rf_transition_readback_frequency_hz = 0;
            board_data->rf_transition_pending    = true;
            board_data->rf_transition_waiting    = false;
            *transaction_id = board_data->rf_transition_current_id;

        }
    });

    if (transition_busy) {
        return BLADERF_ERR_WOULD_BLOCK;
    }

    /* Retire any previously certified sync-RX data before changing RF state.
     * For epoch requests expect_id below also poisons all messages until the
     * FPGA boundary is confirmed. On every failure this invalidation stays
     * latched; only sync_rx_epoch_set_min_timestamp() can clear it. */
    status = sync_rx_epoch_invalidate(&board_data->sync[BLADERF_RX]);
    if (status != 0) {
        _emit_event(dev, board_data, BLADERF_RF_EVT_ERROR,
                    BLADERF_RF_STATE_ERROR,
                    request->target_frequency_hz, 0, 0, status, 0);
        return _fail_transition(dev, board_data, status, NULL);
    }

    _emit_event(dev, board_data, BLADERF_RF_EVT_CONFIG_ACCEPTED,
                BLADERF_RF_STATE_CONFIG_PENDING,
                request->target_frequency_hz, 0, 0, 0, 0);

    /* ADR-0207 §6: arm the FPGA data-plane epoch gate BEFORE the retune so
     * pre-retune samples already in flight through the FIFO writer are
     * suppressed from the moment the SPI write lands. FPGA epoch state
     * survives host close/reopen, so allocate its 8-bit ID from the current
     * FPGA status; transaction_id remains the separate host-side identity.
     *
     * Only armed when the caller actually requires epoch confirmation --
     * BLADERF_RF_REQUIRE_EPOCH_VALID unset means the caller only wants
     * control-plane (PLL+ENSM) confirmation, same opt-in discipline as
     * PLL_LOCKED/ENSM_RX below. If arming or configuring the requested
     * fence fails, abort before the RFIC retune; never downgrade the
     * caller's requested data-plane guarantee. */
    if (required_events_mask & BLADERF_RF_REQUIRE_EPOCH_VALID) {
        uint32_t epoch_status_word = 0;
        uint8_t epoch_id = 0;

        stage_started_ns = _monotonic_ns();
        int epoch_status = _prepare_rx_epoch_id(
            dev, request->timeout_ms ? request->timeout_ms : 1000,
            &epoch_id, &epoch_status_word);
        if (epoch_status == 0) {
            WITH_MUTEX(&dev->lock, {
                board_data->rf_transition_epoch_id = epoch_id;
            });
        }
        /* Fence the host-side parser before changing the FPGA gate. Old USB
         * buffers may already be queued, and the first-valid timestamp is
         * only available after successful RFIC/FPGA completion. Keep the
         * filter installed on every failure path: an uncompleted epoch must
         * never make old IQ visible as current data. */
        if (epoch_status == 0 && board_data->sync[BLADERF_RX].initialized) {
            epoch_status = sync_rx_epoch_expect_id(
                &board_data->sync[BLADERF_RX], epoch_id);
        }
        if (epoch_status == 0) {
            epoch_status = nios_rx_epoch_ctrl_cmd(dev,
                               NIOS_PKT_8x32_RX_EPOCH_CMD_ARM,
                               epoch_id);
        }
        if (epoch_status == 0) {
            epoch_status = _wait_rx_epoch_fenced(
                dev, epoch_id,
                request->timeout_ms ? request->timeout_ms : 1000,
                &epoch_status_word);
        }
        if (epoch_status != 0) {
            log_error("%s: epoch ARM/fence failed: transaction=%u epoch=%u "
                      "status=%d FPGA status=0x%08x\n",
                      __FUNCTION__, board_data->rf_transition_current_id,
                      epoch_id, epoch_status, epoch_status_word);
            /* EPOCH_VALID is a requested completion condition. Do not
             * silently downgrade it to control-plane success: the caller
             * asked for a data-plane fence, so failure to arm that fence
             * must fail the transaction before touching the RFIC. */
            _emit_event(dev, board_data, BLADERF_RF_EVT_ERROR,
                        BLADERF_RF_STATE_ERROR,
                        request->target_frequency_hz, 0, 0,
                        epoch_status, 0);
            return _fail_transition(dev, board_data, epoch_status, NULL);
        }
        log_debug("%s: epoch ARM/fence transaction=%u took %" PRIu64 " us\n",
                  __FUNCTION__, board_data->rf_transition_current_id,
                  (_monotonic_ns() - stage_started_ns) / 1000ULL);

        _emit_event(dev, board_data, BLADERF_RF_EVT_RX_EPOCH_INVALID,
                    BLADERF_RF_STATE_CONFIG_PENDING,
                    request->target_frequency_hz, 0,
                    epoch_status_word, 0, epoch_id);
    }

    _emit_event(dev, board_data, BLADERF_RF_EVT_CONFIG_ACCEPTED,
               BLADERF_RF_STATE_SPI_PROGRAMMING,
               request->target_frequency_hz, 0, 0, 0, 0);

    /* Keep both established RF tuning mechanisms under the same FPGA
     * epoch transaction. Fastlock is executed by NIOS and its response
     * reports whether the profile recall succeeded; the existing board
     * path also performs ad9361_fastlock_exit_foreign() before returning
     * ownership to host-side status/readback. */
    stage_started_ns = _monotonic_ns();
    if (quick_tune != NULL) {
        struct bladerf_quick_tune qt = *quick_tune;
        /* Snapshot the NIOS transport stage timestamps before releasing the
         * device lock, so another control operation cannot overwrite them. */
        WITH_MUTEX(&dev->lock, {
            memset(&dev->nios_retune_trace, 0,
                   sizeof(dev->nios_retune_trace));
            status = dev->board->schedule_retune(
                dev, ch, BLADERF_RETUNE_NOW,
                request->target_frequency_hz, &qt);
            nios_begin_ns = dev->nios_retune_trace.request_begin_ns;
            nios_out_done_ns = dev->nios_retune_trace.usb_out_done_ns;
            nios_response_done_ns = dev->nios_retune_trace.response_done_ns;
            nios_transport_status = dev->nios_retune_trace.status;
            nios_trace_valid = dev->nios_retune_trace.valid;
        });
        if (nios_trace_valid && nios_begin_ns != 0) {
            _emit_event_with_timestamp(dev, board_data,
                BLADERF_RF_EVT_NIOS_RETUNE_BEGIN,
                BLADERF_RF_STATE_SPI_PROGRAMMING,
                request->target_frequency_hz, 0, 0, 0, 0,
                nios_begin_ns, 0, 0);
            if (nios_out_done_ns != 0) {
                _emit_event_with_timestamp(dev, board_data,
                    BLADERF_RF_EVT_NIOS_RETUNE_USB_OUT_DONE,
                    BLADERF_RF_STATE_SPI_PROGRAMMING,
                    request->target_frequency_hz, 0, 0, 0, 0,
                    nios_out_done_ns, 0, 0);
            }
            if (nios_response_done_ns != 0) {
                _emit_event_with_timestamp(dev, board_data,
                    BLADERF_RF_EVT_NIOS_RETUNE_RESPONSE,
                    BLADERF_RF_STATE_SPI_PROGRAMMING,
                    request->target_frequency_hz, 0,
                    (uint32_t)nios_transport_status,
                    status, 0, nios_response_done_ns, 0, 0);
            }
        }
        if (status != 0) {
            log_error("%s: Nios fastlock recall failed: transaction=%u "
                      "target=%" PRIu64 " nios_profile=%u rffe_profile=%u "
                      "status=%d\n",
                      __FUNCTION__, board_data->rf_transition_current_id,
                      (uint64_t)request->target_frequency_hz,
                      qt.nios_profile, qt.rffe_profile, status);
        }
    } else {
        /* Hold device configuration ownership from the transition-specific
         * setter entry through LO programming. Ordinary RX retunes are
         * rejected until wait/abort clears rf_transition_pending. */
        WITH_MUTEX(&dev->lock, {
            /* Observe the complete host retune, including fastlock exit and
             * RF band selection writes that happen before the LO setter. */
            board_data->rf_transition_spi_write_count = 0;
            board_data->rf_transition_spi_first_write_ns = 0;
            board_data->rf_transition_spi_last_write_ns = 0;
            board_data->rf_transition_spi_last_status = 0;
            board_data->rf_transition_spi_trace_enabled = true;
            board_data->rf_transition_setter_active = true;
            status = bladerf_set_frequency_locked(
                dev, ch, request->target_frequency_hz);
            board_data->rf_transition_setter_active = false;
            board_data->rf_transition_spi_trace_enabled = false;
        });
    }

    WITH_MUTEX(&dev->lock, {
        spi_first_write_ns = board_data->rf_transition_spi_first_write_ns;
        spi_last_write_ns = board_data->rf_transition_spi_last_write_ns;
        spi_write_count = board_data->rf_transition_spi_write_count;
        spi_last_status = board_data->rf_transition_spi_last_status;
        board_data->rf_transition_spi_first_write_ns = 0;
        board_data->rf_transition_spi_last_write_ns = 0;
        board_data->rf_transition_spi_write_count = 0;
        board_data->rf_transition_spi_last_status = 0;
    });
    if (spi_write_count != 0 && spi_first_write_ns != 0) {
        _emit_event_with_timestamp(dev, board_data,
            BLADERF_RF_EVT_SPI_WRITE_BEGIN, BLADERF_RF_STATE_SPI_PROGRAMMING,
            request->target_frequency_hz, 0, 0, 0, 0,
            spi_first_write_ns, 0, spi_write_count);
        if (spi_last_write_ns != 0) {
            _emit_event_with_timestamp(dev, board_data,
                BLADERF_RF_EVT_SPI_DONE, BLADERF_RF_STATE_SPI_PROGRAMMING,
                request->target_frequency_hz, 0, (uint32_t)spi_last_status,
                spi_last_status == 0 ? 0 : BLADERF_ERR_UNEXPECTED, 0,
                spi_last_write_ns, 0, spi_write_count);
        }
    }
    if (status != 0) {
        _emit_event(dev, board_data, BLADERF_RF_EVT_ERROR, BLADERF_RF_STATE_ERROR,
                   request->target_frequency_hz, 0, 0, status, 0);
        return _fail_transition(dev, board_data, status, NULL);
    }

    /* This timestamp is the return of the selected host/NIOS tune path.
     * It deliberately does not claim raw SPI completion or PLL lock. */
    _emit_event(dev, board_data, BLADERF_RF_EVT_LO_SET_RETURNED,
                BLADERF_RF_STATE_PLL_ACQUIRING,
                request->target_frequency_hz, 0, 0, 0, 0);
    log_debug("%s: %s RFIC tune transaction=%u took %" PRIu64 " us\n",
              __FUNCTION__, quick_tune != NULL ? "fastlock" : "host",
              board_data->rf_transition_current_id,
              (_monotonic_ns() - stage_started_ns) / 1000ULL);

    /* Real readback, not an assumption that the write succeeded silently
     * (ADR-0207 event contract requires readback_rx_lo_hz to be the
     * observed value). */
    stage_started_ns = _monotonic_ns();
    if (quick_tune != NULL) {
        /* Nios performs the RFIC fastlock recall without updating ADI's
         * host clock-tree cache. Read back the selected RFIC profile's
         * PLL words directly instead of accepting the previous cached LO. */
        status = ad9361_rx_fastlock_get_freq(
            board_data->phy, quick_tune->rffe_profile, &readback_hz);
        if (status != 0) {
            status = BLADERF_ERR_UNEXPECTED;
        }
    } else {
        status = bladerf_get_frequency(dev, ch, &readback_hz);
    }
    log_debug("%s: LO readback transaction=%u took %" PRIu64 " us\n",
              __FUNCTION__, board_data->rf_transition_current_id,
              (_monotonic_ns() - stage_started_ns) / 1000ULL);
    if (status != 0) {
        log_error("%s: LO readback failed: transaction=%u target=%" PRIu64
                  " status=%d\n", __FUNCTION__,
                  board_data->rf_transition_current_id,
                  (uint64_t)request->target_frequency_hz, status);
        _emit_event(dev, board_data, BLADERF_RF_EVT_ERROR, BLADERF_RF_STATE_ERROR,
                   request->target_frequency_hz, 0, 0, status, 0);
        return _fail_transition(dev, board_data, status, NULL);
    }

    /* Frequency readback is part of the transaction contract. AD9361
     * synthesizer quantization can differ slightly from the requested
     * integer-Hz value; use the same 1 kHz tolerance as the scanner's
     * quick-profile validation. */
    if (readback_hz > request->target_frequency_hz
            ? readback_hz - request->target_frequency_hz > 1000
            : request->target_frequency_hz - readback_hz > 1000) {
        log_error("%s: LO readback mismatch: transaction=%u target=%" PRIu64
                  " readback=%" PRIu64 " quick_tune=%s\n", __FUNCTION__,
                  board_data->rf_transition_current_id,
                  (uint64_t)request->target_frequency_hz,
                  (uint64_t)readback_hz,
                  quick_tune != NULL ? "yes" : "no");
        _emit_event(dev, board_data, BLADERF_RF_EVT_ERROR,
                    BLADERF_RF_STATE_ERROR,
                    request->target_frequency_hz, readback_hz, 0,
                    BLADERF_ERR_UNEXPECTED, 0);
        return _fail_transition(dev, board_data,
                                BLADERF_ERR_UNEXPECTED, NULL);
    }

    WITH_MUTEX(&dev->lock, {
        board_data->rf_transition_readback_frequency_hz = readback_hz;
    });

    _emit_event(dev, board_data, BLADERF_RF_EVT_LO_READBACK_MATCH,
                BLADERF_RF_STATE_PLL_ACQUIRING,
                request->target_frequency_hz, readback_hz, 0, 0, 0);

    return 0;
}

int bladerf_rx_transition_begin(struct bladerf *dev, bladerf_channel ch,
                                const struct bladerf_rx_transition_request *request,
                                uint32_t *transaction_id)
{
    return _bladerf_rx_transition_begin(dev, ch, request, NULL,
                                        transaction_id);
}

int bladerf_rx_transition_begin_quick_tune(
    struct bladerf *dev, bladerf_channel ch,
    const struct bladerf_rx_transition_request *request,
    const struct bladerf_quick_tune *quick_tune,
    uint32_t *transaction_id)
{
    if (quick_tune == NULL) {
        return BLADERF_ERR_INVAL;
    }
    return _bladerf_rx_transition_begin(dev, ch, request, quick_tune,
                                        transaction_id);
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
    bool transition_busy = false;
    bool transaction_valid = false;
    uint8_t expected_epoch_id = 0;
    int status;
    uint64_t wait_started_ns;

    if (dev == NULL) {
        return BLADERF_ERR_INVAL;
    }

    board_data = dev->board_data;

    WITH_MUTEX(&dev->lock, {
        if (board_data->rf_transition_pending &&
            board_data->rf_transition_current_id == transaction_id) {
            transaction_valid = true;
            expected_epoch_id = board_data->rf_transition_epoch_id;
            if (board_data->rf_transition_waiting) {
                transition_busy = true;
            } else {
                board_data->rf_transition_waiting = true;
            }
        }
    });

    if (!transaction_valid) {
        log_error("%s: transaction_id %u does not match current %u\n",
                 __FUNCTION__, transaction_id, board_data->rf_transition_current_id);
        return BLADERF_ERR_INVAL;
    }
    if (transition_busy) {
        return BLADERF_ERR_WOULD_BLOCK;
    }

    deadline_ns = _monotonic_ns() + (uint64_t)timeout_ms * 1000000ULL;
    wait_started_ns = _monotonic_ns();
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
                _emit_event(dev, board_data, BLADERF_RF_EVT_ERROR, BLADERF_RF_STATE_ERROR,
                           0, 0, 0, status, 0);
                return _fail_transition(dev, board_data, status, final_event);
            }
            if (pll_reg & VCO_LOCK_BIT) {
                pll_locked = true;
                _emit_event(dev, board_data, BLADERF_RF_EVT_RX_PLL_LOCKED,
                           BLADERF_RF_STATE_PLL_LOCKED, 0, 0, pll_reg, 0, 0);
                break;
            }
            usleep(POLL_INTERVAL_US);
        }

        if (!pll_locked) {
            _emit_event(dev, board_data, BLADERF_RF_EVT_ERROR, BLADERF_RF_STATE_ERROR,
                       0, 0, pll_reg, BLADERF_ERR_TIMEOUT, 0);
            /* Failure detection only -- never report this as valid data. */
            return _fail_transition(dev, board_data, BLADERF_ERR_TIMEOUT,
                                    final_event);
        }
        log_debug("%s: PLL-lock transaction=%u cumulative %" PRIu64 " us\n",
                  __FUNCTION__, transaction_id,
                  (_monotonic_ns() - wait_started_ns) / 1000ULL);
    }

    if (board_data->rf_transition_required_events_mask & BLADERF_RF_REQUIRE_ENSM_RX) {
        while (_monotonic_ns() < deadline_ns) {
            status = _read_rfic_reg(dev, REG_STATE_ADDR, &ensm_reg);
            if (status != 0) {
                _emit_event(dev, board_data, BLADERF_RF_EVT_ERROR, BLADERF_RF_STATE_ERROR,
                           0, 0, 0, status, 0);
                return _fail_transition(dev, board_data, status, final_event);
            }
            uint8_t ensm_state = ensm_reg & ENSM_STATE_MASK;
            if (ensm_state == ENSM_STATE_RX || ensm_state == ENSM_STATE_FDD) {
                ensm_rx = true;
                _emit_event(dev, board_data, BLADERF_RF_EVT_ENSM_RX,
                           BLADERF_RF_STATE_RX_PATH_ARMING, 0, 0, ensm_reg, 0, 0);
                break;
            }
            usleep(POLL_INTERVAL_US);
        }

        if (!ensm_rx) {
            _emit_event(dev, board_data, BLADERF_RF_EVT_ERROR, BLADERF_RF_STATE_ERROR,
                       0, 0, ensm_reg, BLADERF_ERR_TIMEOUT, 0);
            return _fail_transition(dev, board_data, BLADERF_ERR_TIMEOUT,
                                    final_event);
        }
        log_debug("%s: ENSM-RX transaction=%u cumulative %" PRIu64 " us\n",
                  __FUNCTION__, transaction_id,
                  (_monotonic_ns() - wait_started_ns) / 1000ULL);
    }

    /* ADR-0207 §6: the FPGA data-plane epoch gate is now wired up --
     * RX_EPOCH_VALID means the fabric opened admission on a sample edge
     * after the control plane reported transition completion. COMPLETE
     * tells the gate the RFIC transition landed (the gate was armed before
     * retuning); the next admitted sample defines the new epoch boundary.
     * No sample-count discard is used as a correctness condition.
     *
     * Only polled if the caller actually requires it, same opt-in
     * discipline as PLL_LOCKED/ENSM_RX above -- a caller that only wants
     * control-plane confirmation does not pay for this poll, and gets
     * exactly the control-plane-only guarantee it asked for. */
    if (board_data->rf_transition_required_events_mask & BLADERF_RF_REQUIRE_EPOCH_VALID) {
        uint32_t epoch_status_word = 0;
        uint32_t timestamp_lo = 0;
        uint32_t timestamp_hi = 0;
        bool epoch_opened = false;

        status = nios_rx_epoch_ctrl_cmd(dev, NIOS_PKT_8x32_RX_EPOCH_CMD_COMPLETE, 0);
        if (status != 0) {
            _emit_event(dev, board_data, BLADERF_RF_EVT_ERROR, BLADERF_RF_STATE_ERROR,
                       0, 0, 0, status, 0);
            return _fail_transition(dev, board_data, status, final_event);
        }

        while (_monotonic_ns() < deadline_ns) {
            status = nios_rx_epoch_status_read(dev, &epoch_status_word);
            if (status != 0) {
                _emit_event(dev, board_data, BLADERF_RF_EVT_ERROR, BLADERF_RF_STATE_ERROR,
                           0, 0, 0, status, 0);
                return _fail_transition(dev, board_data, status, final_event);
            }

            uint8_t epoch_state = (uint8_t)((epoch_status_word >> NIOS_PKT_8x32_RX_EPOCH_STATUS_STATE_SHIFT)
                                            & NIOS_PKT_8x32_RX_EPOCH_STATUS_STATE_MASK);
            if (epoch_state == NIOS_PKT_8x32_RX_EPOCH_STATE_ACTIVE_NEW ||
                epoch_state == NIOS_PKT_8x32_RX_EPOCH_STATE_ACTIVE) {
                uint8_t reported_epoch_id = (uint8_t)((epoch_status_word >>
                    NIOS_PKT_8x32_RX_EPOCH_STATUS_EPOCH_ID_SHIFT) &
                    NIOS_PKT_8x32_RX_EPOCH_STATUS_EPOCH_ID_MASK);
                if (!nios_pkt_8x32_rx_epoch_status_is_active(
                        epoch_status_word, expected_epoch_id)) {
                    log_error("%s: FPGA reported active stale epoch: "
                              "transaction=%u expected=%u reported=%u "
                              "state=%u status=0x%08x\n",
                              __FUNCTION__, transaction_id,
                              expected_epoch_id, reported_epoch_id,
                              epoch_state, epoch_status_word);
                    _emit_event(dev, board_data, BLADERF_RF_EVT_ERROR,
                                BLADERF_RF_STATE_ERROR, 0, 0,
                                epoch_status_word, BLADERF_ERR_UNEXPECTED, 0);
                    return _fail_transition(dev, board_data,
                                            BLADERF_ERR_UNEXPECTED,
                                            final_event);
                }
                epoch_opened = true;
                break;
            }
            if (epoch_state == NIOS_PKT_8x32_RX_EPOCH_STATE_ERROR) {
                _emit_event(dev, board_data, BLADERF_RF_EVT_ERROR, BLADERF_RF_STATE_ERROR,
                           0, 0, epoch_status_word, BLADERF_ERR_UNEXPECTED, 0);
                return _fail_transition(dev, board_data,
                                        BLADERF_ERR_UNEXPECTED, final_event);
            }
            usleep(POLL_INTERVAL_US);
        }

        if (!epoch_opened) {
            log_error("%s: epoch-valid timeout transaction=%u status=0x%08x "
                      "state=%u epoch=%u\n", __FUNCTION__, transaction_id,
                      epoch_status_word,
                      (unsigned)((epoch_status_word >>
                          NIOS_PKT_8x32_RX_EPOCH_STATUS_STATE_SHIFT) &
                          NIOS_PKT_8x32_RX_EPOCH_STATUS_STATE_MASK),
                      (unsigned)((epoch_status_word >>
                          NIOS_PKT_8x32_RX_EPOCH_STATUS_EPOCH_ID_SHIFT) &
                          NIOS_PKT_8x32_RX_EPOCH_STATUS_EPOCH_ID_MASK));
            _emit_event(dev, board_data, BLADERF_RF_EVT_ERROR, BLADERF_RF_STATE_ERROR,
                       0, 0, epoch_status_word, BLADERF_ERR_TIMEOUT, 0);
            /* Failure detection only -- never report this as valid data. */
            return _fail_transition(dev, board_data, BLADERF_ERR_TIMEOUT,
                                    final_event);
        }
        log_debug("%s: epoch-valid transaction=%u cumulative %" PRIu64 " us\n",
                  __FUNCTION__, transaction_id,
                  (_monotonic_ns() - wait_started_ns) / 1000ULL);

        /* The FPGA latches first_valid_timestamp at the exact sample
         * boundary. Read both halves only after ACTIVE_NEW/ACTIVE is
         * observed; the latched value remains stable until the next ARM. */
        status = nios_rx_epoch_ts_read(dev, false, &timestamp_lo);
        if (status == 0) {
            status = nios_rx_epoch_ts_read(dev, true, &timestamp_hi);
        }
        if (status != 0) {
            _emit_event(dev, board_data, BLADERF_RF_EVT_ERROR,
                        BLADERF_RF_STATE_ERROR, 0, 0,
                        epoch_status_word, status, 0);
            return _fail_transition(dev, board_data, status, final_event);
        }

        /* FPGA timestamp is the authoritative first admitted sample. Install
         * it as a lower bound before reporting transition success; sync_rx()
         * drops stale timestamped messages already queued on USB/host. */
        status = sync_rx_epoch_set_min_timestamp(
            &board_data->sync[BLADERF_RX],
            ((uint64_t)timestamp_hi << 32) | timestamp_lo,
            (uint8_t)((epoch_status_word >>
                NIOS_PKT_8x32_RX_EPOCH_STATUS_EPOCH_ID_SHIFT) &
                NIOS_PKT_8x32_RX_EPOCH_STATUS_EPOCH_ID_MASK));
        if (status != 0) {
            _emit_event(dev, board_data, BLADERF_RF_EVT_ERROR,
                        BLADERF_RF_STATE_ERROR, 0, 0,
                        epoch_status_word, status, 0);
            return _fail_transition(dev, board_data, status, final_event);
        }

        _emit_event_with_timestamp(
                   dev, board_data, BLADERF_RF_EVT_RX_EPOCH_VALID,
                   BLADERF_RF_STATE_RX_DATA_VALID, 0, 0, epoch_status_word, 0,
                   (epoch_status_word >> NIOS_PKT_8x32_RX_EPOCH_STATUS_EPOCH_ID_SHIFT)
                   & NIOS_PKT_8x32_RX_EPOCH_STATUS_EPOCH_ID_MASK,
                   0, ((uint64_t)timestamp_hi << 32) | timestamp_lo, 0);
    } else {
        /* This caller requested control-plane completion only. There was no
         * FPGA sample-boundary proof, so keep the state explicitly invalid. */
        _emit_event(dev, board_data,
                    BLADERF_RF_EVT_CONTROL_PLANE_CONFIRMED,
                    BLADERF_RF_STATE_RX_DATA_INVALID, 0, 0, 0, 0, 0);
    }

    WITH_MUTEX(&dev->lock, {
        board_data->rf_transition_pending = false;
        board_data->rf_transition_waiting = false;
        board_data->rf_transition_setter_active = false;
        if (final_event != NULL) {
            *final_event = board_data->rf_transition_last_event;
        }
    });

    return 0;
}

static bool _is_terminal_event(bladerf_rf_event_type type)
{
    return type == BLADERF_RF_EVT_RX_EPOCH_VALID ||
           type == BLADERF_RF_EVT_RX_FIRST_VALID_HOST_DATA ||
           type == BLADERF_RF_EVT_CONTROL_PLANE_CONFIRMED ||
           type == BLADERF_RF_EVT_RX_DATAPATH_ARMED ||
           type == BLADERF_RF_EVT_ERROR;
}

void bladerf2_rx_transition_note_first_packet(
    struct bladerf *dev, const struct bladerf_metadata *metadata)
{
    struct bladerf2_board_data *board_data;
    struct bladerf_rf_event epoch_event = {0};
    struct bladerf_rf_event event = {0};
    bool found_epoch = false;
    bool already_recorded = false;

    if (dev == NULL || metadata == NULL || !metadata->rx_epoch_id_valid ||
        metadata->actual_count == 0 ||
        (metadata->status & BLADERF_META_STATUS_OVERRUN) != 0) {
        return;
    }

    board_data = dev->board_data;
    if (board_data == NULL) {
        return;
    }

    WITH_MUTEX(&dev->lock, {
        uint32_t retained = board_data->rf_transition_event_count;
        for (uint32_t i = 0; i < retained; ++i) {
            uint32_t slot = (board_data->rf_transition_event_head +
                BLADERF2_RF_EVENT_HISTORY_SIZE - 1 - i) %
                BLADERF2_RF_EVENT_HISTORY_SIZE;
            const struct bladerf_rf_event *candidate =
                &board_data->rf_transition_events[slot];
            if (candidate->event_type == BLADERF_RF_EVT_RX_EPOCH_VALID &&
                candidate->epoch_id == metadata->rx_epoch_id &&
                metadata->timestamp >= candidate->fpga_timestamp) {
                epoch_event = *candidate;
                found_epoch = true;
                break;
            }
        }

        if (found_epoch) {
            for (uint32_t i = 0; i < retained; ++i) {
                uint32_t slot = (board_data->rf_transition_event_head +
                    BLADERF2_RF_EVENT_HISTORY_SIZE - 1 - i) %
                    BLADERF2_RF_EVENT_HISTORY_SIZE;
                const struct bladerf_rf_event *candidate =
                    &board_data->rf_transition_events[slot];
                if (candidate->transaction_id == epoch_event.transaction_id &&
                    candidate->event_type ==
                        BLADERF_RF_EVT_RX_FIRST_VALID_HOST_DATA) {
                    already_recorded = true;
                    break;
                }
            }
        }

        if (found_epoch && !already_recorded) {
            event.host_monotonic_ns = _monotonic_ns();
            event.fpga_timestamp = metadata->timestamp;
            event.transaction_id = epoch_event.transaction_id;
            event.epoch_id = epoch_event.epoch_id;
            event.requested_rx_lo_hz = epoch_event.requested_rx_lo_hz;
            event.readback_rx_lo_hz = epoch_event.readback_rx_lo_hz;
            event.rfic_status = epoch_event.rfic_status;
            event.fpga_state = BLADERF_RF_STATE_RX_DATA_VALID;
            event.event_type = BLADERF_RF_EVT_RX_FIRST_VALID_HOST_DATA;
            event.flags = metadata->status;
            event.error_code = 0;

            board_data->rf_transition_events[
                board_data->rf_transition_event_head] = event;
            board_data->rf_transition_event_head =
                (board_data->rf_transition_event_head + 1) %
                BLADERF2_RF_EVENT_HISTORY_SIZE;
            if (board_data->rf_transition_event_count <
                BLADERF2_RF_EVENT_HISTORY_SIZE) {
                board_data->rf_transition_event_count++;
            }
        }
    });
}

int bladerf_rx_transition_get_events(struct bladerf *dev,
                                     uint32_t transaction_id,
                                     struct bladerf_rf_event *events,
                                     uint32_t capacity,
                                     uint32_t *event_count,
                                     bool *history_complete)
{
    struct bladerf2_board_data *board_data;
    struct bladerf_rf_event first_event = {0};
    struct bladerf_rf_event last_event = {0};
    uint32_t found = 0;
    bool complete = false;

    if (dev == NULL || transaction_id == 0 || event_count == NULL ||
        history_complete == NULL || (capacity != 0 && events == NULL)) {
        return BLADERF_ERR_INVAL;
    }

    board_data = dev->board_data;
    if (board_data == NULL) {
        return BLADERF_ERR_INVAL;
    }

    WITH_MUTEX(&dev->lock, {
        uint32_t retained = board_data->rf_transition_event_count;
        uint32_t oldest = (board_data->rf_transition_event_head +
                           BLADERF2_RF_EVENT_HISTORY_SIZE - retained) %
                          BLADERF2_RF_EVENT_HISTORY_SIZE;

        for (uint32_t i = 0; i < retained; ++i) {
            uint32_t slot = (oldest + i) % BLADERF2_RF_EVENT_HISTORY_SIZE;
            const struct bladerf_rf_event *event =
                &board_data->rf_transition_events[slot];
            if (event->transaction_id != transaction_id) {
                continue;
            }

            if (found == 0) {
                first_event = *event;
            }
            if (found < capacity) {
                events[found] = *event;
            }
            last_event = *event;
            found++;
        }

        complete = found != 0 &&
                   first_event.event_type == BLADERF_RF_EVT_CONFIG_ACCEPTED &&
                   first_event.fpga_state == BLADERF_RF_STATE_CONFIG_PENDING &&
                   _is_terminal_event(last_event.event_type) &&
                   !(board_data->rf_transition_pending &&
                     board_data->rf_transition_current_id == transaction_id);
    });

    *event_count = found;
    *history_complete = complete;

    if (events == NULL && capacity == 0) {
        return 0;
    }
    return found > capacity ? BLADERF_ERR_MEM : 0;
}
