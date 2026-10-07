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
#include <string.h>

#include <libbladeRF.h>

static bool _test_rx_transition_stall(const char *stage)
{
#ifdef BLADERF_ENABLE_TEST_RX_TRANSITION_STALL_INJECTION
    const char *requested = getenv("BLADERF_TEST_RX_TRANSITION_STALL");
    return requested != NULL && strcmp(requested, stage) == 0;
#else
    (void)stage;
    return false;
#endif
}

static void _test_rx_transition_late_observation(const char *stage)
{
#ifdef BLADERF_ENABLE_TEST_RX_TRANSITION_STALL_INJECTION
    const char *requested =
        getenv("BLADERF_TEST_RX_TRANSITION_LATE_OBSERVATION");
    if (requested != NULL && strcmp(requested, stage) == 0) {
        /* Test-only: model a status transaction that starts before the
         * caller's deadline but returns after it. */
        usleep(150000);
    }
#else
    (void)stage;
#endif
}

#include "ad936x.h"
#include "backend/usb/nios_access.h"
#include "board/board.h"
#include "bladerf2_common.h"
#include "common.h"
#include "log.h"
#include "nios_pkt_8x32.h"
#include "rf_transition_policy.h"
#include "streaming/metadata.h"
#include "streaming/sync.h"

/* Byte-exact with sdrscanner/driver/tuner_fault.py::FAULT_REGS and
 * sdrscanner/hs_sweep/rf_transaction_trace.py -- same register map
 * verified from the Python side against this same AD9361 driver. */
#define REG_STATE_ADDR 0x017
#define REG_RX_CP_VCO_LOCK_ADDR 0x247
#define REG_BBPLL_LOCK_STATUS_ADDR 0x05e
#define VCO_LOCK_BIT 0x02
#define BBPLL_LOCK_BIT 0x80
#define ENSM_STATE_MASK 0x0F
#define ENSM_STATE_RX 0x8
#define ENSM_STATE_FDD 0xA
#define RF_LINK_STATUS_RX_FAULT (1u << 14)

/* Test builds can exercise the sticky FPGA-fault handoff without relying on
 * a physical GPIF/FIFO failure. Production always uses the NIOS status word
 * as returned by the device. */
static uint32_t _rx_link_status_for_transition_test(uint32_t status)
{
#ifdef BLADERF_ENABLE_TEST_RX_TRANSITION_STALL_INJECTION
    const char *requested = getenv("BLADERF_TEST_RX_TRANSITION_STALL");
    if (requested != NULL && strcmp(requested, "FPGA_FAULT") == 0) {
        status |= RF_LINK_STATUS_RX_FAULT;
    }
#endif
    return status;
}

static void _emit_event(struct bladerf *dev,
                        struct bladerf2_board_data *board_data,
                        bladerf_rf_event_type type,
                        bladerf_rf_state state,
                        uint64_t requested_hz,
                        uint64_t readback_hz,
                        uint32_t rfic_status,
                        int32_t error_code,
                        uint32_t epoch_id);

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

static bool _deadline_expired(uint64_t deadline_ns)
{
    return bladerf2_rf_transition_deadline_expired(
        _monotonic_ns(), deadline_ns);
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

/* Keep test-only NIOS ABORT failure injection on the shared command path
 * so both failed-transition cleanup and legacy-setter fencing exercise the
 * same failure boundary. Production builds always issue the real command. */
static int _rx_epoch_abort_command(struct bladerf *dev)
{
#ifdef BLADERF_ENABLE_TEST_RX_ABORT_FAULT_INJECTION
    const char *fail_setting = getenv("BLADERF_TEST_FAIL_RX_EPOCH_ABORT");
    if (fail_setting != NULL && fail_setting[0] != '\0') {
        return BLADERF_ERR_UNEXPECTED;
    }
#endif
    return nios_rx_epoch_ctrl_cmd(
        dev, NIOS_PKT_8x32_RX_EPOCH_CMD_ABORT, 0);
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
        status = _rx_epoch_abort_command(dev);
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
    int abort_status = 0;

    if (board_data->rf_transition_required_events_mask &
        BLADERF_RF_REQUIRE_EPOCH_VALID) {
        abort_status = _rx_epoch_abort_command(dev);
    }

    if (abort_status != 0) {
        /* Preserve the primary transition error returned to the caller, but
         * make failed FPGA cleanup explicit in the device-wide event history. */
        _emit_event(dev, board_data, BLADERF_RF_EVT_RX_EPOCH_ABORT_FAILED,
                    BLADERF_RF_STATE_ERROR, 0, 0, 0, abort_status,
                    board_data->rf_transition_epoch_id);
    }

    WITH_MUTEX(&dev->lock, {
        board_data->rf_transition_pending = false;
        board_data->rf_transition_waiting = false;
        board_data->rf_transition_setter_active = false;
        if (final_event != NULL) {
            MUTEX_LOCK(&board_data->rf_transition_event_lock);
            const bool found = bladerf2_rf_event_latest_transition_result_for_transaction(
                board_data->rf_transition_events,
                BLADERF2_RF_EVENT_HISTORY_SIZE,
                board_data->rf_transition_event_head,
                board_data->rf_transition_event_count,
                board_data->rf_transition_current_id, final_event);
            MUTEX_UNLOCK(&board_data->rf_transition_event_lock);
            if (!found) {
                memset(final_event, 0, sizeof(*final_event));
                final_event->transaction_id =
                    board_data->rf_transition_current_id;
                final_event->event_type = BLADERF_RF_EVT_ERROR;
                final_event->fpga_state = BLADERF_RF_STATE_ERROR;
                final_event->error_code = BLADERF_ERR_UNEXPECTED;
            }
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
        event.flags |= BLADERF_RF_EVENT_F_TRANSITION_CHANNEL_VALID;
        if (board_data->rf_transition_current_channel ==
            BLADERF_CHANNEL_RX(1)) {
            event.flags |= BLADERF_RF_EVENT_F_TRANSITION_RX2;
        }
        if (requested_hz == 0) {
            requested_hz = board_data->rf_transition_requested_frequency_hz;
        }
        if (readback_hz == 0) {
            readback_hz = board_data->rf_transition_readback_frequency_hz;
        }
        event.requested_rx_lo_hz = requested_hz;
        event.readback_rx_lo_hz = readback_hz;

        board_data->rf_transition_last_event = event;
        MUTEX_LOCK(&board_data->rx_async_epoch_lock);
        if (type == BLADERF_RF_EVT_RX_EPOCH_VALID) {
            board_data->rf_transition_epoch_certified = false;
            board_data->rf_transition_first_host_data_reported = false;
            memset(&board_data->rf_transition_first_host_data_event, 0,
                   sizeof(board_data->rf_transition_first_host_data_event));
            board_data->rx_async_have_expected_timestamp = false;
        } else if (state != BLADERF_RF_STATE_RX_DATA_VALID) {
            board_data->rf_transition_epoch_certified = false;
            board_data->rf_transition_first_host_data_reported = false;
        }
        /* RX_EPOCH_VALID is durable before the host admission commit below.
         * Keep async admission uncertified until that commit so a late or
         * failed host activation cannot leak IQ after wait() reports error. */
        MUTEX_LOCK(&board_data->rf_transition_event_lock);
        bladerf2_rf_event_append_locked(board_data, &event);
        MUTEX_UNLOCK(&board_data->rf_transition_event_lock);
        COND_SIGNAL(&board_data->rx_async_epoch_cond);
        MUTEX_UNLOCK(&board_data->rx_async_epoch_lock);
        board_data->rf_transition_state = state;
    });
}

struct rx_epoch_admission_context {
    struct bladerf2_board_data *board_data;
    struct bladerf_rf_event event;
    bool admission_lock_held;
};

static int _prepare_async_rx_epoch_admission(void *context,
                                             uint64_t deadline_ns)
{
    struct rx_epoch_admission_context *admission = context;

    if (admission == NULL) {
        return BLADERF_ERR_INVAL;
    }
    return bladerf2_rx_epoch_admission_prepare(
        admission->board_data, &admission->event, deadline_ns,
        &admission->admission_lock_held);
}

static void _finish_async_rx_epoch_admission(void *context)
{
    struct rx_epoch_admission_context *admission = context;
    if (admission != NULL && admission->admission_lock_held) {
        bladerf2_rx_epoch_admission_finish(
            admission->board_data, &admission->admission_lock_held);
    }
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

#define RX_FAULT_MONITOR_INTERVAL_MS 100
#define RF_LINK_STATUS_VERSION_MASK  0xf0000000u
#define RF_LINK_STATUS_VERSION_1     0x10000000u

static bool _test_runtime_rx_fault(void)
{
#ifdef BLADERF_ENABLE_TEST_RX_TRANSITION_STALL_INJECTION
    const char *requested = getenv("BLADERF_TEST_RX_TRANSITION_STALL");
    return requested != NULL && strcmp(requested, "RUNTIME_FPGA_FAULT") == 0;
#else
    return false;
#endif
}

static bool _test_runtime_rx_loss_counter(uint64_t *count)
{
#ifdef BLADERF_ENABLE_TEST_RX_TRANSITION_STALL_INJECTION
    const char *requested = getenv("BLADERF_TEST_RX_TRANSITION_STALL");
    if (count != NULL && requested != NULL &&
        strcmp(requested, "RUNTIME_FPGA_RX_LOSS") == 0) {
        ++*count;
        return true;
    }
#else
    (void)count;
#endif
    return false;
}

static bool _test_runtime_rx_status_unavailable(void)
{
#ifdef BLADERF_ENABLE_TEST_RX_TRANSITION_STALL_INJECTION
    const char *requested = getenv("BLADERF_TEST_RX_TRANSITION_STALL");
    return requested != NULL &&
        (strcmp(requested, "RUNTIME_FPGA_STATUS_READ_FAILURE") == 0 ||
         strcmp(requested, "RUNTIME_FPGA_STATUS_VERSION") == 0);
#else
    return false;
#endif
}

static const char *_runtime_rx_monitor_test_mode(void)
{
#ifdef BLADERF_ENABLE_TEST_RX_TRANSITION_STALL_INJECTION
    return getenv("BLADERF_TEST_RX_TRANSITION_STALL");
#else
    return NULL;
#endif
}

/* Revoke exactly the epoch whose sticky hardware fault was observed. The
 * device lock serializes this reservation against setters and transitions;
 * the epoch lock is the async admission linearization point. */
static void _invalidate_faulted_rx_epoch(struct bladerf *dev,
                                         uint32_t observed_transaction_id,
                                         uint8_t observed_epoch_id,
                                         uint32_t rf_link_status,
                                         uint32_t reason,
                                         int monitor_error)
{
    struct bladerf2_board_data *board_data = dev->board_data;
    struct bladerf_rf_event event = {0};
    bool invalidate = false;
    bool epoch_contract_enabled = false;
    int sync_status = 0;
    int abort_status = 0;

    WITH_MUTEX(&dev->lock, {
        if (!board_data->rf_transition_pending &&
            !board_data->rf_transition_setter_active &&
            board_data->rf_link_dir_on[0]) {
            MUTEX_LOCK(&board_data->rx_async_epoch_lock);
            if (board_data->rf_transition_epoch_contract_enabled &&
                board_data->rf_transition_epoch_certified &&
                bladerf2_rx_fault_observation_is_current(
                    observed_transaction_id,
                    board_data->rf_transition_current_id,
                    observed_epoch_id,
                    board_data->rf_transition_certified_epoch_id)) {
                board_data->rf_transition_epoch_certified = false;
                board_data->rx_async_have_expected_timestamp = false;
                epoch_contract_enabled = true;
                invalidate = true;
            }
            MUTEX_UNLOCK(&board_data->rx_async_epoch_lock);
            if (invalidate) {
                board_data->rf_transition_setter_active = true;
            }
        }
    });

    if (!invalidate) {
        return;
    }

    /* Revoke sync reads before publishing the fault and issuing the NIOS
     * ABORT request. A reader starting in that control-transfer window must
     * not consume a queued buffer under the old certified generation. */
    sync_rx_epoch_revoke_delivery(&board_data->sync[BLADERF_RX]);
    bladerf2_rx_data_withheld_reset(dev);
    event.host_monotonic_ns = _monotonic_ns();
    event.epoch_id = observed_epoch_id;
    event.requested_rx_lo_hz =
        board_data->rf_transition_requested_frequency_hz;
    event.readback_rx_lo_hz = board_data->rf_transition_readback_frequency_hz;
    event.fpga_state = BLADERF_RF_STATE_RX_DATA_INVALID;
    event.event_type = BLADERF_RF_EVT_RX_DATA_INVALIDATED;
    event.flags = reason;
    event.rfic_status = rf_link_status;
    event.error_code = monitor_error;
    bladerf2_rf_event_append_rx_invalidation(
        board_data, &event,
        bladerf2_rx_current_transition_channel_event_flags(board_data));

    if (epoch_contract_enabled) {
        abort_status = _rx_epoch_abort_command(dev);
    }
    if (abort_status != 0) {
        event.host_monotonic_ns = _monotonic_ns();
        event.event_type = BLADERF_RF_EVT_RX_EPOCH_ABORT_FAILED;
        event.fpga_state = BLADERF_RF_STATE_ERROR;
        event.error_code = abort_status;
        bladerf2_rf_event_append(board_data, &event);
    }

    /* Publish and close the FPGA gate before touching sync state. A reader
     * may hold sync->lock while waiting for a buffer; waiting for that lock
     * here would otherwise delay the first durable invalidation event until
     * the stream timeout. */
    sync_status = sync_rx_epoch_invalidate(&board_data->sync[BLADERF_RX]);
    if (sync_status != 0) {
        event.host_monotonic_ns = _monotonic_ns();
        event.event_type = BLADERF_RF_EVT_ERROR;
        event.fpga_state = BLADERF_RF_STATE_ERROR;
        event.error_code = sync_status;
        bladerf2_rf_event_append(board_data, &event);
    }

    event.host_monotonic_ns = _monotonic_ns();
    event.event_type = BLADERF_RF_EVT_RX_STREAM_OVERRUN;
    event.fpga_state = BLADERF_RF_STATE_RX_DATA_INVALID;
    event.flags = BLADERF_RF_STREAM_STATUS_OVERRUN |
                  BLADERF_RF_STREAM_STATUS_RUNTIME_STATE_FAULT |
                  bladerf2_rx_current_transition_channel_event_flags(
                      board_data);
    event.error_code = sync_status != 0 ? sync_status :
        (monitor_error != 0 ? monitor_error : BLADERF_ERR_UNEXPECTED);
    bladerf2_rf_event_append(board_data, &event);

    WITH_MUTEX(&dev->lock, {
        board_data->rf_transition_state = BLADERF_RF_STATE_RX_DATA_INVALID;
        board_data->rf_transition_last_event = event;
        board_data->rf_transition_setter_active = false;
    });
}

static void *rx_fault_monitor_task(void *arg)
{
    struct bladerf *dev = arg;
    struct bladerf2_board_data *board_data = dev->board_data;

    for (;;) {
        bool stop;
        MUTEX_LOCK(&board_data->rx_fault_monitor_lock);
        if (!board_data->rx_fault_monitor_stop) {
            (void)COND_TIMED_WAIT(&board_data->rx_fault_monitor_cond,
                                  &board_data->rx_fault_monitor_lock,
                                  RX_FAULT_MONITOR_INTERVAL_MS);
        }
        stop = board_data->rx_fault_monitor_stop;
        MUTEX_UNLOCK(&board_data->rx_fault_monitor_lock);
        if (stop) {
            break;
        }

        bool should_poll = false;
        bool epoch_certified = false;
        uint32_t transaction_id = 0;
        uint8_t epoch_id = 0;
        uint32_t rf_link_status = 0;
        uint32_t rx_fault_causes = 0;
        bool rx_fault_causes_valid = false;
        uint8_t pll_status = 0;
        uint8_t ensm_status = 0;
        uint8_t bbpll_status = 0;
        uint32_t rffe_status = 0;
        uint8_t expected_rx_channel_mask = 0;
        bool expected_rx_channel_mask_valid = false;
        int rx_channel_status = 0;
        uint64_t rx_loss_count = 0;
        int status = 0;
        int loss_count_status = 0;
        int rfic_status = 0;
        int rx_fault_causes_status = 0;

        /* NIOS bulk control and AD9361 SPI requests share serialization with
         * configuration traffic. Hold dev->lock only across these short
         * status reads, and never perform them from a libusb stream callback. */
        WITH_MUTEX(&dev->lock, {
            MUTEX_LOCK(&board_data->rx_async_epoch_lock);
            epoch_certified =
                board_data->rf_transition_epoch_contract_enabled &&
                board_data->rf_transition_epoch_certified;
            epoch_id = board_data->rf_transition_certified_epoch_id;
            expected_rx_channel_mask = board_data->rx_channel_enable_mask;
            expected_rx_channel_mask_valid =
                board_data->rx_channel_enable_mask_valid;
            MUTEX_UNLOCK(&board_data->rx_async_epoch_lock);
            transaction_id = board_data->rf_transition_current_id;
            should_poll = board_data->state == STATE_INITIALIZED &&
                board_data->rf_link_dir_on[0] && epoch_certified &&
                !board_data->rf_transition_pending &&
                !board_data->rf_transition_setter_active;
            if (should_poll) {
                rx_channel_status = dev->backend->rffe_control_read(
                    dev, &rffe_status);
                status = nios_rf_link_status_read(dev, &rf_link_status);
                if (status == 0 && dev->board->get_loss_event_count != NULL) {
                    loss_count_status = dev->board->get_loss_event_count(
                        dev, BLADERF_RX, &rx_loss_count);
                } else if (status == 0) {
                    loss_count_status = BLADERF_ERR_UNSUPPORTED;
                }
                if (status == 0 &&
                    (rf_link_status & RF_LINK_STATUS_VERSION_MASK) ==
                        RF_LINK_STATUS_VERSION_1 &&
                    (rf_link_status & RF_LINK_STATUS_RX_FAULT) == 0) {
                    rfic_status = _read_rfic_reg(
                        dev, REG_RX_CP_VCO_LOCK_ADDR, &pll_status);
                    if (rfic_status == 0) {
                        rfic_status = _read_rfic_reg(
                            dev, REG_STATE_ADDR, &ensm_status);
                    }
                    if (rfic_status == 0) {
                        rfic_status = _read_rfic_reg(
                            dev, REG_BBPLL_LOCK_STATUS_ADDR, &bbpll_status);
                    }
                } else if (status == 0 &&
                           (rf_link_status & RF_LINK_STATUS_VERSION_MASK) ==
                               RF_LINK_STATUS_VERSION_1 &&
                           (rf_link_status & RF_LINK_STATUS_RX_FAULT) != 0) {
                    rx_fault_causes_status = nios_rx_fault_causes_read(
                        dev, &rx_fault_causes);
                    rx_fault_causes_valid = rx_fault_causes_status == 0 &&
                        (rx_fault_causes & 0x1fu) != 0;
                }
            }
        });

        if (!should_poll) {
            continue;
        }
        const char *test_mode = _runtime_rx_monitor_test_mode();
        if (test_mode != NULL &&
            strcmp(test_mode, "RUNTIME_RX_CHANNEL_STATUS_READ_FAILURE") == 0) {
            rx_channel_status = BLADERF_ERR_IO;
        } else if (test_mode != NULL &&
                   strcmp(test_mode, "RUNTIME_RX_CHANNEL_MASK_CHANGED") == 0) {
            rffe_status ^= (1u << RFFE_CONTROL_MIMO_RX_EN_0);
        }
        const uint8_t observed_rx_channel_mask =
            (uint8_t)(((rffe_status >> RFFE_CONTROL_MIMO_RX_EN_0) & 1u) |
                      (((rffe_status >> RFFE_CONTROL_MIMO_RX_EN_1) & 1u) << 1));
        const enum bladerf2_rx_channel_mask_observation channel_observation =
            bladerf2_rx_channel_mask_observation(
                rx_channel_status == 0, expected_rx_channel_mask_valid,
                expected_rx_channel_mask, observed_rx_channel_mask);
        if (channel_observation == BLADERF2_RX_CHANNEL_MASK_UNAVAILABLE) {
            const int monitor_error = rx_channel_status != 0
                ? rx_channel_status : BLADERF_ERR_UNEXPECTED;
            _invalidate_faulted_rx_epoch(
                dev, transaction_id, epoch_id,
                rx_channel_status != 0 ? (uint32_t)rx_channel_status
                                       : expected_rx_channel_mask,
                BLADERF_RF_INVALIDATE_RX_CHANNEL_STATUS_UNAVAILABLE,
                monitor_error);
            continue;
        }
        if (channel_observation == BLADERF2_RX_CHANNEL_MASK_CHANGED) {
            _invalidate_faulted_rx_epoch(
                dev, transaction_id, epoch_id,
                ((uint32_t)expected_rx_channel_mask << 8) |
                    observed_rx_channel_mask,
                BLADERF_RF_INVALIDATE_RX_CHANNEL_STATE_CHANGED, 0);
            continue;
        }
        if (_test_runtime_rx_status_unavailable()) {
            const char *requested =
                getenv("BLADERF_TEST_RX_TRANSITION_STALL");
            if (strcmp(requested, "RUNTIME_FPGA_STATUS_READ_FAILURE") == 0) {
                status = BLADERF_ERR_IO;
            } else {
                rf_link_status &= ~RF_LINK_STATUS_VERSION_MASK;
            }
        }
        if (status != 0 ||
            (rf_link_status & RF_LINK_STATUS_VERSION_MASK) !=
                RF_LINK_STATUS_VERSION_1) {
            const int monitor_error = status != 0 ? status : BLADERF_ERR_UNEXPECTED;
            _invalidate_faulted_rx_epoch(
                dev, transaction_id, epoch_id, rf_link_status,
                BLADERF_RF_INVALIDATE_FPGA_STATUS_UNAVAILABLE,
                monitor_error);
            continue;
        }
        if (loss_count_status == 0) {
            (void)_test_runtime_rx_loss_counter(&rx_loss_count);
            bool report_loss = false;
            bool counter_reset = false;
            WITH_MUTEX(&dev->lock, {
                if (!board_data->rx_fpga_loss_count_valid) {
                    board_data->rx_fpga_loss_count_last = rx_loss_count;
                    board_data->rx_fpga_loss_count_valid = true;
                } else if (rx_loss_count <
                           board_data->rx_fpga_loss_count_last) {
                    board_data->rx_fpga_loss_count_last = rx_loss_count;
                    counter_reset = true;
                } else if (rx_loss_count >
                           board_data->rx_fpga_loss_count_last) {
                    board_data->rx_fpga_loss_count_last = rx_loss_count;
                    report_loss = true;
                }
            });
            if (report_loss) {
                bladerf2_rx_fpga_loss(dev, epoch_id, rx_loss_count);
            }
            if (counter_reset) {
                _invalidate_faulted_rx_epoch(
                    dev, transaction_id, epoch_id, (uint32_t)rx_loss_count,
                    BLADERF_RF_INVALIDATE_FPGA_RX_LOSS_STATUS_UNAVAILABLE,
                    BLADERF_ERR_UNEXPECTED);
                continue;
            }
        } else {
            _invalidate_faulted_rx_epoch(
                dev, transaction_id, epoch_id, (uint32_t)loss_count_status,
                BLADERF_RF_INVALIDATE_FPGA_RX_LOSS_STATUS_UNAVAILABLE,
                loss_count_status);
            continue;
        }
        if (test_mode != NULL &&
            strcmp(test_mode, "RUNTIME_RFIC_STATUS_READ_FAILURE") == 0) {
            rfic_status = BLADERF_ERR_UNEXPECTED;
        } else if (test_mode != NULL &&
                   strcmp(test_mode, "RUNTIME_RFIC_PLL_UNLOCKED") == 0) {
            pll_status &= (uint8_t)~VCO_LOCK_BIT;
        } else if (test_mode != NULL &&
                   strcmp(test_mode, "RUNTIME_RFIC_ENSM_NOT_RX") == 0) {
            ensm_status = 0x05;
        } else if (test_mode != NULL &&
                   strcmp(test_mode, "RUNTIME_RFIC_BBPLL_UNLOCKED") == 0) {
            bbpll_status &= (uint8_t)~BBPLL_LOCK_BIT;
        }
        if (_test_runtime_rx_fault()) {
            rf_link_status |= RF_LINK_STATUS_RX_FAULT;
        }
        if ((rf_link_status & RF_LINK_STATUS_RX_FAULT) != 0) {
            _invalidate_faulted_rx_epoch(
                dev, transaction_id, epoch_id,
                rx_fault_causes_valid
                    ? (0x80000000u | (rx_fault_causes & 0x1fu))
                    : rf_link_status,
                BLADERF_RF_INVALIDATE_FPGA_RX_FAULT, 0);
            continue;
        }
        if (rfic_status != 0) {
            _invalidate_faulted_rx_epoch(
                dev, transaction_id, epoch_id,
                ((uint32_t)ensm_status << 8) | pll_status,
                BLADERF_RF_INVALIDATE_RFIC_STATUS_UNAVAILABLE,
                rfic_status);
            continue;
        }
        if ((pll_status & VCO_LOCK_BIT) == 0) {
            _invalidate_faulted_rx_epoch(
                dev, transaction_id, epoch_id, pll_status,
                BLADERF_RF_INVALIDATE_RFIC_PLL_UNLOCKED, 0);
            continue;
        }
        uint8_t ensm_state = ensm_status & ENSM_STATE_MASK;
        if (ensm_state != ENSM_STATE_RX && ensm_state != ENSM_STATE_FDD) {
            _invalidate_faulted_rx_epoch(
                dev, transaction_id, epoch_id, ensm_status,
                BLADERF_RF_INVALIDATE_RFIC_ENSM_NOT_RX, 0);
            continue;
        }
        if ((bbpll_status & BBPLL_LOCK_BIT) == 0) {
            _invalidate_faulted_rx_epoch(
                dev, transaction_id, epoch_id, bbpll_status,
                BLADERF_RF_INVALIDATE_RFIC_BBPLL_UNLOCKED, 0);
        }
    }

    return NULL;
}

int bladerf2_rx_fault_monitor_start(struct bladerf *dev)
{
    struct bladerf2_board_data *board_data;
    int status;

    if (dev == NULL || dev->board_data == NULL) {
        return BLADERF_ERR_INVAL;
    }
    board_data = dev->board_data;
    MUTEX_INIT(&board_data->rx_fault_monitor_lock);
    status = COND_INIT(&board_data->rx_fault_monitor_cond);
    if (status != 0) {
        MUTEX_DESTROY(&board_data->rx_fault_monitor_lock);
        return BLADERF_ERR_UNEXPECTED;
    }
    board_data->rx_fault_monitor_sync_initialized = true;
    board_data->rx_fault_monitor_stop = false;
    status = THREAD_CREATE(&board_data->rx_fault_monitor_thread,
                           rx_fault_monitor_task, dev);
    if (status != THREAD_SUCCESS) {
        COND_DESTROY(&board_data->rx_fault_monitor_cond);
        MUTEX_DESTROY(&board_data->rx_fault_monitor_lock);
        board_data->rx_fault_monitor_sync_initialized = false;
        return BLADERF_ERR_UNEXPECTED;
    }
    board_data->rx_fault_monitor_started = true;
    return 0;
}

void bladerf2_rx_fault_monitor_stop(struct bladerf *dev)
{
    struct bladerf2_board_data *board_data;

    if (dev == NULL || dev->board_data == NULL) {
        return;
    }
    board_data = dev->board_data;
    if (board_data->rx_fault_monitor_started) {
        MUTEX_LOCK(&board_data->rx_fault_monitor_lock);
        board_data->rx_fault_monitor_stop = true;
        COND_SIGNAL(&board_data->rx_fault_monitor_cond);
        MUTEX_UNLOCK(&board_data->rx_fault_monitor_lock);
        THREAD_JOIN(board_data->rx_fault_monitor_thread, NULL);
        board_data->rx_fault_monitor_started = false;
    }
    if (board_data->rx_fault_monitor_sync_initialized) {
        COND_DESTROY(&board_data->rx_fault_monitor_cond);
        MUTEX_DESTROY(&board_data->rx_fault_monitor_lock);
        board_data->rx_fault_monitor_sync_initialized = false;
    }
}

int bladerf2_rx_data_invalidate(struct bladerf *dev, bladerf_channel ch,
                                uint32_t reason)
{
    struct bladerf2_board_data *board_data;
    struct bladerf_rf_event event = {0};
    bool data_epoch_enabled;
    int status;

    if (BLADERF_CHANNEL_IS_TX(ch)) {
        return 0;
    }
    if (dev == NULL || dev->board_data == NULL || reason == 0) {
        return BLADERF_ERR_INVAL;
    }
    board_data = dev->board_data;

    /* Serialize the entire public setter, not just this notification. A
     * transition begin must not slip between invalidation and the RFIC write. */
    WITH_MUTEX(&dev->lock, {
        if (board_data->rf_transition_pending ||
            board_data->rf_transition_setter_active) {
            log_debug("%s: invalidation blocked (transition_pending=%u "
                      "setter_active=%u state=%u transaction=%u)\n",
                      __FUNCTION__, board_data->rf_transition_pending,
                      board_data->rf_transition_setter_active,
                      board_data->rf_transition_state,
                      board_data->rf_transition_current_id);
            status = BLADERF_ERR_WOULD_BLOCK;
        } else {
            board_data->rf_transition_setter_active = true;
            status = 0;
        }
    });
    if (status != 0) {
        return status;
    }

    /* Revoke async admission before any USB control transaction. The sync
     * parser has its own fence, but async RX does not necessarily have an
     * initialized sync stream; using sync_rx_epoch_filter_enabled() here
     * skipped the FPGA ABORT for async-only META consumers. This lock is the
     * linearization point shared with async buffer admission, so a buffer
     * parsed before this point is rejected at its commit check. The RX LO and
     * epoch are shared by RX1/RX2, so revoke the common certificate. */
    MUTEX_LOCK(&board_data->rx_async_epoch_lock);
    data_epoch_enabled = board_data->rf_transition_epoch_contract_enabled;
    board_data->rf_transition_epoch_certified = false;
    board_data->rx_async_have_expected_timestamp = false;
    MUTEX_UNLOCK(&board_data->rx_async_epoch_lock);
    bladerf2_rx_data_withheld_reset(dev);
    log_debug("%s: reserved RX reconfiguration (reason=0x%x)\n",
              __FUNCTION__, reason);

    status = sync_rx_epoch_invalidate(&board_data->sync[BLADERF_RX]);
    if (status != 0) {
        log_debug("%s: sync epoch invalidation failed: %s\n",
                  __FUNCTION__, bladerf_strerror(status));
    }

    event.host_monotonic_ns = _monotonic_ns();
    event.transaction_id = 0; /* invalidation is not a transition transaction */
    event.epoch_id = board_data->rf_transition_epoch_id;
    event.requested_rx_lo_hz = board_data->rf_transition_readback_frequency_hz;
    event.readback_rx_lo_hz = board_data->rf_transition_readback_frequency_hz;
    event.fpga_state = BLADERF_RF_STATE_RX_DATA_INVALID;
    event.event_type = BLADERF_RF_EVT_RX_DATA_INVALIDATED;
    event.flags = reason;
    event.error_code = status;

    WITH_MUTEX(&dev->lock, {
        board_data->rf_transition_state = BLADERF_RF_STATE_RX_DATA_INVALID;
        bladerf2_rf_event_append_rx_invalidation(
            board_data, &event,
            bladerf2_rx_transition_channel_event_flags(ch, true));
        /* Keep the reservation through the actual legacy setter only when
         * the invalidation/fence succeeded. */
        if (status != 0) {
            board_data->rf_transition_setter_active = false;
        }
    });

    if (status == 0 && data_epoch_enabled) {
#ifdef BLADERF_ENABLE_TEST_RX_ABORT_FAULT_INJECTION
        /* Hold the hardware fence open after host revocation so a live async
         * stream can verify that no stale IQ crosses this race window. */
        const char *delay_setting =
            getenv("BLADERF_TEST_DELAY_RX_EPOCH_ABORT_MS");
        if (delay_setting != NULL && delay_setting[0] != '\0') {
            char *end = NULL;
            unsigned long delay_ms = strtoul(delay_setting, &end, 10);
            if (end != delay_setting && *end == '\0' && delay_ms <= 5000) {
                usleep((useconds_t)(delay_ms * 1000UL));
            }
        }
#endif
        /* Close the FPGA gate for both sync and async epoch consumers before
         * the legacy setter mutates the RFIC. ABORT is fail-closed (ERROR). */
        status = _rx_epoch_abort_command(dev);
        if (status != 0) {
            event.host_monotonic_ns = _monotonic_ns();
            event.event_type = BLADERF_RF_EVT_RX_EPOCH_ABORT_FAILED;
            event.error_code = status;
            WITH_MUTEX(&dev->lock, {
                board_data->rf_transition_setter_active = false;
                bladerf2_rf_event_append(board_data, &event);
            });
            return status;
        }
    }

    return status;
}

void bladerf2_rx_reconfigure_complete(struct bladerf *dev,
                                      bladerf_channel ch)
{
    struct bladerf2_board_data *board_data;

    if (dev == NULL || BLADERF_CHANNEL_IS_TX(ch) || dev->board_data == NULL) {
        return;
    }
    board_data = dev->board_data;
    WITH_MUTEX(&dev->lock, {
        board_data->rf_transition_setter_active = false;
        log_debug("%s: released RX reconfiguration reservation\n",
                  __FUNCTION__);
    });
}

void bladerf2_rx_stream_overrun(struct bladerf *dev, uint32_t source_flags)
{
    struct bladerf2_board_data *board_data;
    struct bladerf_rf_event event = {0};

    if (dev == NULL || dev->board_data == NULL) {
        return;
    }
    board_data = dev->board_data;
    event.host_monotonic_ns = _monotonic_ns();
    event.event_type = BLADERF_RF_EVT_RX_STREAM_OVERRUN;
    event.flags = BLADERF_RF_STREAM_STATUS_OVERRUN | source_flags;
    WITH_MUTEX(&dev->lock, {
        MUTEX_LOCK(&board_data->rx_async_epoch_lock);
        event.flags |= bladerf2_rx_transition_channel_event_flags(
            board_data->rf_transition_current_channel,
            board_data->rf_transition_epoch_contract_enabled);
        board_data->rx_async_data_withheld_active = true;
        MUTEX_UNLOCK(&board_data->rx_async_epoch_lock);
        event.transaction_id = 0;
        event.epoch_id = board_data->rf_transition_epoch_id;
        event.requested_rx_lo_hz =
            board_data->rf_transition_requested_frequency_hz;
        event.readback_rx_lo_hz =
            board_data->rf_transition_readback_frequency_hz;
        event.fpga_state = board_data->rf_transition_state;
        bladerf2_rf_event_append(board_data, &event);
        board_data->rf_transition_last_event = event;
    });
}

struct rx_fpga_loss_event_context {
    struct bladerf2_board_data *board_data;
    struct bladerf_rf_event event;
};

static void _publish_rx_fpga_loss_event(void *context)
{
    struct rx_fpga_loss_event_context *loss = context;

    MUTEX_LOCK(&loss->board_data->rf_transition_event_lock);
    bladerf2_rf_event_append_locked(loss->board_data, &loss->event);
    MUTEX_UNLOCK(&loss->board_data->rf_transition_event_lock);
}

void bladerf2_rx_fpga_loss(struct bladerf *dev, uint8_t epoch_id,
                           uint64_t loss_count)
{
    struct bladerf2_board_data *board_data;
    struct rx_fpga_loss_event_context loss = {0};

    if (dev == NULL || dev->board_data == NULL) {
        return;
    }
    board_data = dev->board_data;

    loss.board_data = board_data;
    loss.event.host_monotonic_ns = _monotonic_ns();
    loss.event.epoch_id = epoch_id;
    loss.event.rfic_status = (uint32_t)loss_count;
    loss.event.fpga_state = BLADERF_RF_STATE_RX_DATA_INVALID;
    loss.event.event_type = BLADERF_RF_EVT_RX_STREAM_OVERRUN;
    loss.event.flags = BLADERF_RF_STREAM_STATUS_OVERRUN |
                       BLADERF_RF_STREAM_STATUS_FPGA_RX_LOSS |
                       bladerf2_rx_current_transition_channel_event_flags(
                           board_data);
    loss.event.transaction_id = 0;
    WITH_MUTEX(&dev->lock, {
        loss.event.requested_rx_lo_hz =
            board_data->rf_transition_requested_frequency_hz;
        loss.event.readback_rx_lo_hz =
            board_data->rf_transition_readback_frequency_hz;
    });

    /* Store the event while holding the sync queue lock: once a sync reader
     * can observe the pending overrun, its reason is already in RF history. */
    sync_rx_report_fpga_loss(&board_data->sync[BLADERF_RX],
                             _publish_rx_fpga_loss_event, &loss);

    WITH_MUTEX(&dev->lock, {
        MUTEX_LOCK(&board_data->rx_async_epoch_lock);
        board_data->rx_async_data_withheld_active = true;
        MUTEX_UNLOCK(&board_data->rx_async_epoch_lock);
        board_data->rf_transition_last_event = loss.event;
    });
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
    bool epochless_async_stream_active = false;
    bool sync_format_unsupported = false;
    bool rx_x2_layout_unsupported = false;
    uint64_t stage_started_ns;
    uint64_t spi_first_write_ns = 0;
    uint64_t spi_last_write_ns = 0;
    uint32_t spi_write_count = 0;
    int spi_last_status = 0;
    uint64_t nios_begin_ns = 0;
    uint64_t nios_out_done_ns = 0;
    uint64_t nios_response_done_ns = 0;
    uint64_t nios_duration_ticks = 0;
    int nios_transport_status = 0;
    bool nios_trace_valid = false;
    bool nios_duration_valid = false;

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

    WITH_MUTEX(&dev->lock, {
        if (board_data->rf_transition_pending ||
            board_data->rf_transition_setter_active) {
            transition_busy = true;
        } else if (bladerf2_rx_epoch_transition_blocked_by_async_format(
                       (required_events_mask &
                        BLADERF_RF_REQUIRE_EPOCH_VALID) != 0,
                       board_data->rx_async_epochless_stream_count)) {
            /* An active legacy async consumer cannot identify the FPGA
             * boundary. Reject before allocating a transaction, enabling
             * the epoch contract, or touching the RFIC. */
            epochless_async_stream_active = true;
        } else if (bladerf2_rx_x1_consumer_blocks_x2_transition(
                       (required_events_mask &
                        BLADERF_RF_REQUIRE_RX_X2_HOST_DATA) != 0,
                       board_data->rx_async_x1_stream_count,
                       board_data->sync[BLADERF_RX].initialized &&
                       board_data->sync[BLADERF_RX].stream_config.layout ==
                           BLADERF_RX_X1)) {
            /* A paired-data requirement cannot be met by an active RX_X1
             * consumer. Reject before mutating RF state. */
            rx_x2_layout_unsupported = true;
        } else if ((required_events_mask &
                    BLADERF_RF_REQUIRE_EPOCH_VALID) &&
                   board_data->sync[BLADERF_RX].initialized &&
                   (board_data->sync[BLADERF_RX].stream_config.layout &
                    BLADERF_DIRECTION_MASK) == BLADERF_RX &&
                   board_data->sync[BLADERF_RX].stream_config.format !=
                       BLADERF_FORMAT_SC16_Q11_META &&
                   board_data->sync[BLADERF_RX].stream_config.format !=
                       BLADERF_FORMAT_SC8_Q7_META) {
            /* sync_config updates this immutable stream description under
             * dev->lock. Inspect it here instead of taking sync->lock: a
             * blocked sync_rx must first be cancelled by the transaction
             * invalidation below. */
            sync_format_unsupported = true;
        } else {
            board_data->rf_transition_next_id =
                bladerf2_rx_transition_next_transaction_id(
                    board_data->rf_transition_next_id);
            board_data->rf_transition_current_id = board_data->rf_transition_next_id;
            {
                const uint32_t timing_slot =
                    board_data->rf_transition_current_id %
                    BLADERF2_RF_EVENT_HISTORY_SIZE;
                board_data->rf_transition_nios_timing[timing_slot].transaction_id =
                    board_data->rf_transition_current_id;
                board_data->rf_transition_nios_timing[timing_slot].duration_ticks = 0;
                board_data->rf_transition_nios_timing[timing_slot].nios_retune_observed = false;
                board_data->rf_transition_nios_timing[timing_slot].duration_valid = false;
            }
            board_data->rf_transition_required_events_mask = required_events_mask;
            MUTEX_LOCK(&board_data->rx_async_epoch_lock);
            board_data->rf_transition_current_channel = ch;
            board_data->rf_transition_rx_x2_host_data_required =
                (required_events_mask &
                 BLADERF_RF_REQUIRE_RX_X2_HOST_DATA) != 0;
            board_data->rf_transition_rx_x2_host_data_transaction_id =
                board_data->rf_transition_rx_x2_host_data_required
                    ? board_data->rf_transition_current_id : 0;
            if (required_events_mask & BLADERF_RF_REQUIRE_EPOCH_VALID) {
                board_data->rf_transition_epoch_contract_enabled = true;
                board_data->rf_transition_first_host_data_required =
                    (required_events_mask &
                     BLADERF_RF_REQUIRE_FIRST_HOST_DATA) != 0;
                board_data->rf_transition_first_host_data_deadline_ns = 0;
                board_data->rf_transition_first_host_data_reported = false;
                board_data->rf_transition_first_host_data_failure = 0;
                memset(&board_data->rf_transition_first_host_data_event, 0,
                       sizeof(board_data->rf_transition_first_host_data_event));
            }
            MUTEX_UNLOCK(&board_data->rx_async_epoch_lock);
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
    if (epochless_async_stream_active) {
        return BLADERF_ERR_UNSUPPORTED;
    }
    if (rx_x2_layout_unsupported) {
        bladerf2_rx_layout_unsupported(dev, BLADERF_RX_X1, false);
        return BLADERF_ERR_UNSUPPORTED;
    }
    if (sync_format_unsupported) {
        return BLADERF_ERR_UNSUPPORTED;
    }

    /* Retire any previously certified sync-RX data before changing RF state.
     * For epoch requests expect_id below also poisons all messages until the
     * FPGA boundary is confirmed. On every failure this invalidation stays
     * latched; only sync_rx_epoch_set_min_timestamp() can clear it. */
    status = sync_rx_epoch_invalidate(&board_data->sync[BLADERF_RX]);
    bladerf2_rx_data_withheld_reset(dev);
    if (status != 0) {
        _emit_event(dev, board_data, BLADERF_RF_EVT_ERROR,
                    BLADERF_RF_STATE_ERROR,
                    request->target_frequency_hz, 0, 0, status, 0);
        return _fail_transition(dev, board_data, status, NULL);
    }

    /* A legacy scheduled RX recall can outlive this handle-side transaction
     * and move the LO after the newly certified epoch opens. Retire all
     * previously queued RX recalls before arming the next epoch. Older FPGA
     * images without the queue capability have nothing to cancel. */
    if (dev->board->cancel_scheduled_retunes != NULL) {
        WITH_MUTEX(&dev->lock, {
            status = dev->board->cancel_scheduled_retunes(dev, ch);
        });
        if (status != 0 && status != BLADERF_ERR_UNSUPPORTED) {
            _emit_event(dev, board_data, BLADERF_RF_EVT_ERROR,
                        BLADERF_RF_STATE_ERROR,
                        request->target_frequency_hz, 0, 0, status, 0);
            return _fail_transition(dev, board_data, status, NULL);
        }
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
#ifdef BLADERF_ENABLE_TEST_RX_ABORT_FAULT_INJECTION
            /* Test-only failure at the FPGA fence boundary. This exercises
             * the fail-closed path before any RFIC write; production builds
             * compile out the environment-variable check entirely. */
            const char *fail_arm = getenv("BLADERF_TEST_FAIL_RX_EPOCH_ARM");
            if (fail_arm != NULL && fail_arm[0] != '\0') {
                epoch_status = BLADERF_ERR_UNEXPECTED;
            } else
#endif
            {
                epoch_status = nios_rx_epoch_ctrl_cmd(
                    dev, NIOS_PKT_8x32_RX_EPOCH_CMD_ARM, epoch_id);
            }
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
            board_data->rf_transition_scheduling_quick_tune = true;
            status = dev->board->schedule_retune(
                dev, ch, BLADERF_RETUNE_NOW,
                request->target_frequency_hz, &qt);
            board_data->rf_transition_scheduling_quick_tune = false;
            nios_begin_ns = dev->nios_retune_trace.request_begin_ns;
            nios_out_done_ns = dev->nios_retune_trace.usb_out_done_ns;
            nios_response_done_ns = dev->nios_retune_trace.response_done_ns;
            nios_duration_ticks = dev->nios_retune_trace.device_duration_ticks;
            nios_transport_status = dev->nios_retune_trace.status;
            nios_trace_valid = dev->nios_retune_trace.valid;
            nios_duration_valid = dev->nios_retune_trace.device_duration_valid;
            if (nios_trace_valid) {
                const uint32_t timing_slot =
                    board_data->rf_transition_current_id %
                    BLADERF2_RF_EVENT_HISTORY_SIZE;
                board_data->rf_transition_nios_timing[timing_slot].transaction_id =
                    board_data->rf_transition_current_id;
                board_data->rf_transition_nios_timing[timing_slot].duration_ticks =
                    nios_duration_ticks;
                board_data->rf_transition_nios_timing[timing_slot].nios_retune_observed = true;
                board_data->rf_transition_nios_timing[timing_slot].duration_valid =
                    nios_duration_valid;
            }
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
            board_data->rf_transition_scheduling_quick_tune = false;
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

static int _wait_for_first_host_data(
    struct bladerf2_board_data *board_data, uint32_t transaction_id,
    uint64_t deadline_ns, struct bladerf_rf_event *host_data_event)
{
    int status = 0;

    MUTEX_LOCK(&board_data->rx_async_epoch_lock);
    for (;;) {
        if (board_data->rf_transition_first_host_data_failure != 0) {
            status = board_data->rf_transition_first_host_data_failure;
            break;
        }
        if (board_data->rf_transition_first_host_data_reported &&
            board_data->rf_transition_first_host_data_event.transaction_id ==
                transaction_id &&
            board_data->rf_transition_first_host_data_event.event_type ==
                BLADERF_RF_EVT_RX_FIRST_VALID_HOST_DATA) {
            const struct bladerf_rf_event *candidate =
                &board_data->rf_transition_first_host_data_event;
            if (!bladerf2_rx_first_host_data_before_deadline(
                    candidate->host_monotonic_ns, deadline_ns)) {
                status = BLADERF_ERR_TIMEOUT;
            } else {
                *host_data_event = *candidate;
            }
            break;
        }

        if (!board_data->rf_transition_epoch_certified ||
            board_data->rf_transition_certified_epoch_event.transaction_id !=
                transaction_id) {
            status = BLADERF_ERR_UNEXPECTED;
            break;
        }

        const uint64_t now_ns = _monotonic_ns();
        if (now_ns == 0) {
            status = BLADERF_ERR_UNEXPECTED;
            break;
        }
        if (_deadline_expired(deadline_ns)) {
            status = BLADERF_ERR_TIMEOUT;
            break;
        }

        uint64_t remaining_ns = deadline_ns - now_ns;
        uint64_t remaining_ms = (remaining_ns + 999999ULL) / 1000000ULL;
        if (remaining_ms == 0) {
            remaining_ms = 1;
        } else if (remaining_ms > UINT32_MAX) {
            remaining_ms = UINT32_MAX;
        }
        const int wait_status = COND_TIMED_WAIT(
            &board_data->rx_async_epoch_cond,
            &board_data->rx_async_epoch_lock, (uint32_t)remaining_ms);
        if (wait_status != THREAD_SUCCESS &&
            wait_status != THREAD_TIMEOUT) {
            status = BLADERF_ERR_UNEXPECTED;
            break;
        }
    }
    MUTEX_UNLOCK(&board_data->rx_async_epoch_lock);
    return status;
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
    bool first_host_data_required = false;
    uint8_t expected_epoch_id = 0;
    struct bladerf_rf_event first_host_data_event = {0};
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
            first_host_data_required =
                (board_data->rf_transition_required_events_mask &
                 BLADERF_RF_REQUIRE_FIRST_HOST_DATA) != 0;
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
    if (board_data->rf_transition_required_events_mask &
        BLADERF_RF_REQUIRE_FIRST_HOST_DATA) {
        MUTEX_LOCK(&board_data->rx_async_epoch_lock);
        board_data->rf_transition_first_host_data_deadline_ns = deadline_ns;
        MUTEX_UNLOCK(&board_data->rx_async_epoch_lock);
    }
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
            _test_rx_transition_late_observation("LATE_PLL");
            /* The RFIC register transaction can itself block on SPI/USB.
             * Its observation is not timely merely because polling began
             * before the deadline. */
            if (_deadline_expired(deadline_ns)) {
                break;
            }
            if (_test_rx_transition_stall("PLL")) {
                pll_reg &= (uint8_t)~VCO_LOCK_BIT;
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
            _test_rx_transition_late_observation("LATE_ENSM");
            if (_deadline_expired(deadline_ns)) {
                break;
            }
            if (_test_rx_transition_stall("ENSM")) {
                ensm_reg &= (uint8_t)~ENSM_STATE_MASK;
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

    if (board_data->rf_transition_required_events_mask &
        BLADERF_RF_REQUIRE_BBPLL_LOCKED) {
        uint8_t bbpll_status = 0;
        bool bbpll_locked = false;
        while (_monotonic_ns() < deadline_ns) {
            status = _read_rfic_reg(dev, REG_BBPLL_LOCK_STATUS_ADDR,
                                    &bbpll_status);
            if (status != 0) {
                _emit_event_with_timestamp(
                    dev, board_data, BLADERF_RF_EVT_RX_DATA_INVALIDATED,
                    BLADERF_RF_STATE_RX_DATA_INVALID, 0, 0, bbpll_status,
                    status, expected_epoch_id, 0, 0,
                    BLADERF_RF_INVALIDATE_RFIC_STATUS_UNAVAILABLE);
                _emit_event(dev, board_data, BLADERF_RF_EVT_ERROR,
                            BLADERF_RF_STATE_ERROR, 0, 0,
                            bbpll_status, status, expected_epoch_id);
                return _fail_transition(dev, board_data, status, final_event);
            }
            _test_rx_transition_late_observation("LATE_BBPLL");
            if (_deadline_expired(deadline_ns)) {
                break;
            }
            if (_test_rx_transition_stall("BBPLL")) {
                bbpll_status &= (uint8_t)~BBPLL_LOCK_BIT;
            }
            if ((bbpll_status & BBPLL_LOCK_BIT) != 0) {
                bbpll_locked = true;
                _emit_event(dev, board_data,
                            BLADERF_RF_EVT_RX_BBPLL_LOCKED,
                            BLADERF_RF_STATE_RX_PATH_ARMING, 0, 0,
                            bbpll_status, 0, expected_epoch_id);
                break;
            }
            usleep(POLL_INTERVAL_US);
        }
        if (!bbpll_locked) {
            _emit_event_with_timestamp(
                dev, board_data, BLADERF_RF_EVT_RX_DATA_INVALIDATED,
                BLADERF_RF_STATE_RX_DATA_INVALID, 0, 0, bbpll_status,
                BLADERF_ERR_TIMEOUT, expected_epoch_id, 0, 0,
                BLADERF_RF_INVALIDATE_RFIC_BBPLL_UNLOCKED);
            _emit_event(dev, board_data, BLADERF_RF_EVT_ERROR,
                        BLADERF_RF_STATE_ERROR, 0, 0,
                        bbpll_status, BLADERF_ERR_TIMEOUT,
                        expected_epoch_id);
            return _fail_transition(dev, board_data, BLADERF_ERR_TIMEOUT,
                                    final_event);
        }
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
        uint32_t status_reads = 0;
        uint32_t status_poll_sleeps = 0;
        uint64_t status_read_total_ns = 0;
        uint64_t status_read_max_ns = 0;
        bool epoch_opened = false;

        uint64_t complete_elapsed_ns;
        uint64_t timestamp_lo_elapsed_ns = 0;
        uint64_t timestamp_hi_elapsed_ns;
        uint64_t fence_elapsed_ns;
        uint64_t stage_begin_ns = _monotonic_ns();
        if (_test_rx_transition_stall("EPOCH")) {
            /* Keep the real FPGA gate fenced for the positive-timeout test. */
            status = 0;
        } else {
            status = nios_rx_epoch_ctrl_cmd(
                dev, NIOS_PKT_8x32_RX_EPOCH_CMD_COMPLETE, 0);
        }
        complete_elapsed_ns = _monotonic_ns() - stage_begin_ns;
        if (status != 0) {
            _emit_event(dev, board_data, BLADERF_RF_EVT_ERROR, BLADERF_RF_STATE_ERROR,
                       0, 0, 0, status, 0);
            return _fail_transition(dev, board_data, status, final_event);
        }
        _test_rx_transition_late_observation("LATE_COMPLETE");
        if (_deadline_expired(deadline_ns)) {
            _emit_event(dev, board_data, BLADERF_RF_EVT_ERROR,
                        BLADERF_RF_STATE_ERROR, 0, 0, 0,
                        BLADERF_ERR_TIMEOUT, expected_epoch_id);
            return _fail_transition(dev, board_data, BLADERF_ERR_TIMEOUT,
                                    final_event);
        }

        while (_monotonic_ns() < deadline_ns) {
            const uint64_t read_begin_ns = _monotonic_ns();
            if (!board_data->rx_epoch_snapshot_capability_checked &&
                getenv("BLADERF_TEST_RX_EPOCH_FORCE_LEGACY_SNAPSHOT") != NULL) {
                board_data->rx_epoch_snapshot_capability_checked = true;
                board_data->rx_epoch_snapshot_supported = false;
                log_debug("%s: forcing legacy epoch status/timestamp reads "
                          "for qualification\n", __FUNCTION__);
            }
            if (!board_data->rx_epoch_snapshot_capability_checked ||
                board_data->rx_epoch_snapshot_supported) {
                status = nios_rx_epoch_status_snapshot_read(
                    dev, &epoch_status_word, &timestamp_lo);
                status_reads++;
                if (status == BLADERF_ERR_UNSUPPORTED) {
                    board_data->rx_epoch_snapshot_capability_checked = true;
                    board_data->rx_epoch_snapshot_supported = false;
                    status = nios_rx_epoch_status_read(
                        dev, &epoch_status_word);
                    status_reads++;
                } else if (status == 0) {
                    board_data->rx_epoch_snapshot_capability_checked = true;
                    board_data->rx_epoch_snapshot_supported = true;
                }
            } else {
                status = nios_rx_epoch_status_read(dev, &epoch_status_word);
                status_reads++;
            }
            const uint64_t read_elapsed_ns = _monotonic_ns() - read_begin_ns;
            status_read_total_ns += read_elapsed_ns;
            if (read_elapsed_ns > status_read_max_ns) {
                status_read_max_ns = read_elapsed_ns;
            }
            if (status != 0) {
                _emit_event(dev, board_data, BLADERF_RF_EVT_ERROR, BLADERF_RF_STATE_ERROR,
                           0, 0, 0, status, 0);
                return _fail_transition(dev, board_data, status, final_event);
            }
            _test_rx_transition_late_observation("LATE_EPOCH");
            /* A slow NIOS status query may finish after the deadline. Never
             * accept ACTIVE_NEW based on that late snapshot. */
            if (_deadline_expired(deadline_ns)) {
                break;
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
            status_poll_sleeps++;
            usleep(POLL_INTERVAL_US);
        }

        log_debug("%s: FPGA epoch status transaction=%u reads=%u sleeps=%u "
                  "read_total_us=%.3f read_max_us=%.3f opened=%u status=0x%08x\n",
                  __FUNCTION__, transaction_id, status_reads,
                  status_poll_sleeps, status_read_total_ns / 1000.0,
                  status_read_max_ns / 1000.0, epoch_opened,
                  epoch_status_word);

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
        if (!board_data->rx_epoch_snapshot_supported) {
            stage_begin_ns = _monotonic_ns();
            status = nios_rx_epoch_ts_read(dev, false, &timestamp_lo);
            timestamp_lo_elapsed_ns = _monotonic_ns() - stage_begin_ns;
        }
        stage_begin_ns = _monotonic_ns();
        if (status == 0) {
            status = nios_rx_epoch_ts_read(dev, true, &timestamp_hi);
        }
        timestamp_hi_elapsed_ns = _monotonic_ns() - stage_begin_ns;
        if (status != 0) {
            _emit_event(dev, board_data, BLADERF_RF_EVT_ERROR,
                        BLADERF_RF_STATE_ERROR, 0, 0,
                        epoch_status_word, status, 0);
            return _fail_transition(dev, board_data, status, final_event);
        }
        _test_rx_transition_late_observation("LATE_TIMESTAMP");
        if (_deadline_expired(deadline_ns)) {
            _emit_event(dev, board_data, BLADERF_RF_EVT_ERROR,
                        BLADERF_RF_STATE_ERROR, 0, 0, epoch_status_word,
                        BLADERF_ERR_TIMEOUT, expected_epoch_id);
            return _fail_transition(dev, board_data, BLADERF_ERR_TIMEOUT,
                                    final_event);
        }

        /* The fabric maintains a sticky, direction-specific RX fault
         * aggregate for watchdog, GPIF, protocol, speed, and FIFO-abort
         * failures. Do not publish EPOCH_VALID from the gate alone if the
         * transport/data writer faulted while the epoch was opening. Check
         * after the timestamp snapshot and before installing the host-side
         * certificate. */
        uint32_t rf_link_status = 0;
        status = nios_rf_link_status_read(dev, &rf_link_status);
        if (status != 0) {
            _emit_event(dev, board_data, BLADERF_RF_EVT_ERROR,
                        BLADERF_RF_STATE_ERROR, 0, 0,
                        epoch_status_word, status, expected_epoch_id);
            return _fail_transition(dev, board_data, status, final_event);
        }
        _test_rx_transition_late_observation("LATE_LINK_STATUS");
        if (_deadline_expired(deadline_ns)) {
            _emit_event(dev, board_data, BLADERF_RF_EVT_ERROR,
                        BLADERF_RF_STATE_ERROR, 0, 0, rf_link_status,
                        BLADERF_ERR_TIMEOUT, expected_epoch_id);
            return _fail_transition(dev, board_data, BLADERF_ERR_TIMEOUT,
                                    final_event);
        }
        rf_link_status = _rx_link_status_for_transition_test(rf_link_status);
        if ((rf_link_status & RF_LINK_STATUS_RX_FAULT) != 0) {
            log_error("%s: FPGA RX fault before epoch certification: "
                      "transaction=%u status=0x%08x epoch=%u\n",
                      __FUNCTION__, transaction_id, rf_link_status,
                      expected_epoch_id);
            _emit_event(dev, board_data, BLADERF_RF_EVT_ERROR,
                        BLADERF_RF_STATE_ERROR, 0, 0,
                        rf_link_status, BLADERF_ERR_UNEXPECTED,
                        expected_epoch_id);
            return _fail_transition(dev, board_data,
                                    BLADERF_ERR_UNEXPECTED, final_event);
        }

        /* Stage the exact FPGA boundary while keeping sync RX invalidated.
         * Publish RX_EPOCH_VALID before committing host admission so a
         * concurrent reader cannot return first-host-data ahead of the epoch
         * event. Activation checks the deadline under sync->lock and holds
         * the async admission lock through sync-latch release; failure leaves
         * both IQ paths fenced. */
        _test_rx_transition_late_observation("LATE_HOST_FENCE");
        stage_begin_ns = _monotonic_ns();
        status = sync_rx_epoch_stage_min_timestamp_before_deadline(
            &board_data->sync[BLADERF_RX],
            ((uint64_t)timestamp_hi << 32) | timestamp_lo,
            (uint8_t)((epoch_status_word >>
                NIOS_PKT_8x32_RX_EPOCH_STATUS_EPOCH_ID_SHIFT) &
                NIOS_PKT_8x32_RX_EPOCH_STATUS_EPOCH_ID_MASK),
            deadline_ns);
        fence_elapsed_ns = _monotonic_ns() - stage_begin_ns;
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
                   0, ((uint64_t)timestamp_hi << 32) | timestamp_lo,
                   BLADERF_RF_EVENT_F_FPGA_TIMESTAMP_VALID);
        _test_rx_transition_late_observation("LATE_HOST_ACTIVATE");
        struct rx_epoch_admission_context admission = {0};
        admission.board_data = board_data;
        admission.event.host_monotonic_ns = _monotonic_ns();
        admission.event.transaction_id = transaction_id;
        admission.event.fpga_timestamp =
            ((uint64_t)timestamp_hi << 32) | timestamp_lo;
        admission.event.epoch_id = expected_epoch_id;
        admission.event.requested_rx_lo_hz =
            board_data->rf_transition_requested_frequency_hz;
        admission.event.readback_rx_lo_hz =
            board_data->rf_transition_readback_frequency_hz;
        admission.event.rfic_status = epoch_status_word;
        admission.event.fpga_state = BLADERF_RF_STATE_RX_DATA_VALID;
        admission.event.event_type = BLADERF_RF_EVT_RX_EPOCH_VALID;
        admission.event.flags = BLADERF_RF_EVENT_F_FPGA_TIMESTAMP_VALID;
        status = sync_rx_epoch_activate_with_admission_before_deadline(
            &board_data->sync[BLADERF_RX], deadline_ns,
            _prepare_async_rx_epoch_admission,
            _finish_async_rx_epoch_admission, &admission);
        fence_elapsed_ns = _monotonic_ns() - stage_begin_ns;
        if (status != 0) {
            _emit_event(dev, board_data, BLADERF_RF_EVT_ERROR,
                        BLADERF_RF_STATE_ERROR, 0, 0,
                        epoch_status_word, status, expected_epoch_id);
            return _fail_transition(dev, board_data, status, final_event);
        }
        log_debug("%s: epoch handoff transaction=%u complete_us=%.3f "
                  "status_reads=%u status_total_us=%.3f timestamp_lo_us=%.3f "
                  "timestamp_hi_us=%.3f sync_fence_us=%.3f\n",
                  __FUNCTION__, transaction_id,
                  complete_elapsed_ns / 1000.0, status_reads,
                  status_read_total_ns / 1000.0,
                  timestamp_lo_elapsed_ns / 1000.0,
                  timestamp_hi_elapsed_ns / 1000.0,
                  fence_elapsed_ns / 1000.0);
    } else {
        /* This caller requested control-plane completion only. There was no
         * FPGA sample-boundary proof, so keep the state explicitly invalid. */
        if (_deadline_expired(deadline_ns)) {
            _emit_event(dev, board_data, BLADERF_RF_EVT_ERROR,
                        BLADERF_RF_STATE_ERROR, 0, 0, 0,
                        BLADERF_ERR_TIMEOUT, expected_epoch_id);
            return _fail_transition(dev, board_data, BLADERF_ERR_TIMEOUT,
                                    final_event);
        }
        _emit_event(dev, board_data,
                    BLADERF_RF_EVT_CONTROL_PLANE_CONFIRMED,
                    BLADERF_RF_STATE_RX_DATA_INVALID, 0, 0, 0, 0, 0);
    }

    if (first_host_data_required) {
        status = _wait_for_first_host_data(
            board_data, transaction_id, deadline_ns,
            &first_host_data_event);
        if (status != 0) {
            _emit_event(dev, board_data, BLADERF_RF_EVT_ERROR,
                        BLADERF_RF_STATE_ERROR, 0, 0, 0, status,
                        expected_epoch_id);
            return _fail_transition(dev, board_data, status, final_event);
        }
    }

    WITH_MUTEX(&dev->lock, {
        board_data->rf_transition_pending = false;
        board_data->rf_transition_waiting = false;
        board_data->rf_transition_setter_active = false;
        if (final_event != NULL) {
            bool found;
            if (first_host_data_required) {
                *final_event = first_host_data_event;
                found = first_host_data_event.transaction_id == transaction_id &&
                    first_host_data_event.event_type ==
                        BLADERF_RF_EVT_RX_FIRST_VALID_HOST_DATA;
            } else {
                MUTEX_LOCK(&board_data->rf_transition_event_lock);
                found = bladerf2_rf_event_latest_transition_result_for_transaction(
                    board_data->rf_transition_events,
                    BLADERF2_RF_EVENT_HISTORY_SIZE,
                    board_data->rf_transition_event_head,
                    board_data->rf_transition_event_count,
                    transaction_id, final_event);
                MUTEX_UNLOCK(&board_data->rf_transition_event_lock);
            }
            if (!found) {
                memset(final_event, 0, sizeof(*final_event));
                final_event->transaction_id = transaction_id;
                final_event->event_type = BLADERF_RF_EVT_ERROR;
                final_event->fpga_state = BLADERF_RF_STATE_ERROR;
                final_event->error_code = BLADERF_ERR_UNEXPECTED;
            }
        }
    });

    return final_event != NULL && final_event->error_code ==
           BLADERF_ERR_UNEXPECTED &&
           final_event->event_type == BLADERF_RF_EVT_ERROR
        ? BLADERF_ERR_UNEXPECTED : 0;
}

void bladerf2_rx_transition_note_first_packet_epoch_locked(
    struct bladerf *dev, const struct bladerf_metadata *metadata,
    bladerf_channel_layout layout)
{
    if (dev != NULL && dev->board_data != NULL) {
        /* Caller holds rx_async_epoch_lock. The durable epoch snapshot, not
         * the bounded event ring, links this packet to its transition. */
        bladerf2_rx_data_note_first_packet_locked(dev->board_data, metadata,
                                                  layout);
    }
}

void bladerf2_rx_transition_note_first_packet(
    struct bladerf *dev, const struct bladerf_metadata *metadata,
    bladerf_channel_layout layout)
{
    struct bladerf2_board_data *board_data;

    if (dev == NULL || dev->board_data == NULL || metadata == NULL) {
        return;
    }
    board_data = dev->board_data;

    /* Sync RX calls this only after returning from its parser. Async RX uses
     * the epoch-locked helper at the admission commit point. */
    MUTEX_LOCK(&board_data->rx_async_epoch_lock);
    bladerf2_rx_transition_note_first_packet_epoch_locked(dev, metadata,
                                                         layout);
    MUTEX_UNLOCK(&board_data->rx_async_epoch_lock);
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
    uint32_t found = 0;
    bool complete = false;
    bool transition_terminal_event_found = false;

    if (dev == NULL || transaction_id == 0 || event_count == NULL ||
        history_complete == NULL || (capacity != 0 && events == NULL)) {
        return BLADERF_ERR_INVAL;
    }
    CHECK_BOARD_IS_BLADERF2(dev);

    board_data = dev->board_data;
    if (board_data == NULL) {
        return BLADERF_ERR_INVAL;
    }

    WITH_MUTEX(&dev->lock, {
        MUTEX_LOCK(&board_data->rf_transition_event_lock);
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
            if (bladerf2_rf_event_is_transition_terminal(
                    event->event_type)) {
                transition_terminal_event_found = true;
            }
            found++;
        }

        complete = found != 0 &&
                   first_event.event_type == BLADERF_RF_EVT_CONFIG_ACCEPTED &&
                   first_event.fpga_state == BLADERF_RF_STATE_CONFIG_PENDING &&
                   transition_terminal_event_found &&
                   !(board_data->rf_transition_pending &&
                     board_data->rf_transition_current_id == transaction_id);
        MUTEX_UNLOCK(&board_data->rf_transition_event_lock);
    });

    *event_count = found;
    *history_complete = complete;

    if (events == NULL && capacity == 0) {
        return 0;
    }
    return found > capacity ? BLADERF_ERR_MEM : 0;
}

int bladerf_rf_events_get_since(struct bladerf *dev, uint64_t after_sequence,
                                struct bladerf_rf_event *events,
                                uint32_t capacity, uint32_t *event_count,
                                uint64_t *next_sequence,
                                bool *history_complete)
{
    struct bladerf2_board_data *board_data;
    uint32_t copied = 0;
    bool complete = true;
    int status = 0;

    if (dev == NULL || event_count == NULL || next_sequence == NULL ||
        history_complete == NULL || (capacity != 0 && events == NULL)) {
        return BLADERF_ERR_INVAL;
    }
    CHECK_BOARD_IS_BLADERF2(dev);
    board_data = dev->board_data;
    if (board_data == NULL) {
        return BLADERF_ERR_INVAL;
    }

    *next_sequence = after_sequence;
    MUTEX_LOCK(&board_data->rf_transition_event_lock);
    {
        uint32_t retained = board_data->rf_transition_event_count;
        uint64_t current_sequence = board_data->rf_transition_event_sequence;
        uint32_t oldest = (board_data->rf_transition_event_head +
                           BLADERF2_RF_EVENT_HISTORY_SIZE - retained) %
                          BLADERF2_RF_EVENT_HISTORY_SIZE;
        uint64_t oldest_sequence = retained != 0 ?
            board_data->rf_transition_event_sequences[oldest] :
            board_data->rf_transition_event_sequence + 1;

        if (!bladerf2_rf_event_cursor_is_valid(after_sequence,
                                               current_sequence)) {
            /* Resynchronize to the current tail and explicitly report an
             * incomplete history. Never let a fabricated future cursor
             * suppress later invalidation events while appearing complete. */
            complete = false;
            *next_sequence = current_sequence;
            status = BLADERF_ERR_INVAL;
        } else if (retained != 0 && after_sequence < oldest_sequence - 1) {
            complete = false;
        }

        for (uint32_t i = 0; complete && i < retained; ++i) {
            uint32_t slot = (oldest + i) % BLADERF2_RF_EVENT_HISTORY_SIZE;
            uint64_t sequence =
                board_data->rf_transition_event_sequences[slot];
            if (sequence <= after_sequence) {
                continue;
            }
            if (copied < capacity) {
                events[copied++] = board_data->rf_transition_events[slot];
                *next_sequence = sequence;
            } else {
                complete = false;
                break;
            }
        }
    }
    MUTEX_UNLOCK(&board_data->rf_transition_event_lock);

    *event_count = copied;
    *history_complete = complete;
    return status != 0 ? status : (complete ? 0 : BLADERF_ERR_MEM);
}

int bladerf_rx_transition_get_nios_timing(
    struct bladerf *dev, uint32_t transaction_id,
    struct bladerf_rx_transition_nios_timing *timing)
{
    struct bladerf2_board_data *board_data;
    uint32_t slot;

    if (dev == NULL || transaction_id == 0 || timing == NULL) {
        return BLADERF_ERR_INVAL;
    }
    board_data = dev->board_data;
    if (board_data == NULL) {
        return BLADERF_ERR_INVAL;
    }

    memset(timing, 0, sizeof(*timing));
    slot = transaction_id % BLADERF2_RF_EVENT_HISTORY_SIZE;
    WITH_MUTEX(&dev->lock, {
        if (board_data->rf_transition_nios_timing[slot].transaction_id ==
            transaction_id) {
            timing->transaction_retained = true;
            timing->nios_retune_observed =
                board_data->rf_transition_nios_timing[slot].nios_retune_observed;
            timing->device_duration_valid =
                board_data->rf_transition_nios_timing[slot].duration_valid;
            timing->device_duration_ticks =
                board_data->rf_transition_nios_timing[slot].duration_ticks;
        }
    });
    return 0;
}
