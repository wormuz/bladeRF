#ifndef BLADERF_NIOS_RFIC_CALIBRATION_POLICY_H_
#define BLADERF_NIOS_RFIC_CALIBRATION_POLICY_H_

#include <stdbool.h>
#include <stdint.h>

#define RFIC_RX_RF_PORT_COUNT 3u

enum rfic_calibration_poll_action {
    RFIC_CALIBRATION_POLL_AGAIN,
    RFIC_CALIBRATION_POLL_FINISH_COMMAND,
    RFIC_CALIBRATION_POLL_RESUME_COMMAND,
    RFIC_CALIBRATION_POLL_FAIL_COMMAND,
};

enum rfic_rfdc_poll_action {
    RFIC_RFDC_POLL_AGAIN,
    RFIC_RFDC_POLL_RESUME_RX_ENABLE,
    RFIC_RFDC_POLL_COMPLETE,
};

enum rfic_sample_rate_cache_action {
    RFIC_SAMPLE_RATE_CACHE_PRESERVE,
    RFIC_SAMPLE_RATE_CACHE_INVALIDATE,
};

/* Keep a failed RFIC operation as the primary result; if the operation
 * succeeded but restoration failed, the cleanup error becomes the command
 * failure. The cleanup result is still reported separately by the caller. */
static inline int32_t rfic_operation_status_with_cleanup(
    int32_t operation_status, int32_t cleanup_status)
{
    return operation_status < 0 ? operation_status : cleanup_status;
}

/* A rejected, side-effect-free preflight must not erase valid readback
 * caches. Once a transaction is active, or a failed start left the RF
 * bandwidth state invalid, callers must keep both shared-rate caches closed. */
static inline enum rfic_sample_rate_cache_action
rfic_sample_rate_cache_action(bool transaction_active, bool start_failed,
                              bool rf_state_known)
{
    if (transaction_active || (start_failed && !rf_state_known)) {
        return RFIC_SAMPLE_RATE_CACHE_INVALIDATE;
    }
    return RFIC_SAMPLE_RATE_CACHE_PRESERVE;
}

static inline bool rfic_sample_rate_restore_cache_after_rollback(
    bool rollback_complete, bool bandwidth_cache_valid, bool rf_state_known)
{
    return rollback_complete && bandwidth_cache_valid && rf_state_known;
}

/* A yielded calibration may be the middle stage of a command whose final
 * hardware commit must still run (for example, TXQ before RFFE TX enable).
 * Keep that command RUNNING until its continuation commits or fails. */
static inline enum rfic_calibration_poll_action
rfic_calibration_poll_action(int32_t status, bool resume_command)
{
    if (status > 0) {
        return RFIC_CALIBRATION_POLL_AGAIN;
    }
    if (status < 0) {
        return RFIC_CALIBRATION_POLL_FAIL_COMMAND;
    }
    return resume_command ? RFIC_CALIBRATION_POLL_RESUME_COMMAND
                          : RFIC_CALIBRATION_POLL_FINISH_COMMAND;
}

static inline bool rfic_tx_enable_continuation_matches(
    uint32_t expected_channel, uint32_t expected_enable,
    uint32_t channel, uint32_t enable)
{
    return expected_channel == channel && expected_enable == (enable > 0);
}

/* RX enable may resume its final RFFE commit only after the matching RFDC
 * job completed and the AD9361 calibration state is still known. */
static inline bool rfic_rx_enable_resume_commit_allowed(
    int32_t calibration_status, bool continuation_matches,
    bool calibration_state_known)
{
    return calibration_status == 0 && continuation_matches &&
           calibration_state_known;
}

/* RFDC completion does not retire RX enable: its saved RFFE commit must run
 * through the original queue callback. Other RFDC operations finish in the
 * poll worker itself. */
static inline enum rfic_rfdc_poll_action rfic_rfdc_poll_action(
    int32_t calibration_status, bool resume_rx_enable)
{
    if (calibration_status == 1) {
        return RFIC_RFDC_POLL_AGAIN;
    }
    return resume_rx_enable ? RFIC_RFDC_POLL_RESUME_RX_ENABLE
                            : RFIC_RFDC_POLL_COMPLETE;
}

/* A calibration whose completion is unknown poisons the AD9361 context.
 * Permit only INIT/OFF teardown and the INIT/ON recovery path, which performs
 * a full RESETB pulse before a new driver context is initialized. */
static inline bool rfic_calibration_command_allowed(bool faulted,
						    bool is_init_command)
{
	return !faulted || is_init_command;
}

/* Raw AD9361 register/gain-table writes bypass the queued command policy.
 * Reject them both when state requires recovery and while another RFIC job
 * owns the device across a callback or cooperative poll. */
static inline bool rfic_direct_mutation_allowed(bool faulted,
                                                bool device_owned)
{
    return !faulted && !device_owned;
}

/* Raw register writes bypass the ADI driver's frequency, calibration,
 * CTRL_OUT, filter, and ENSM state. When a NIOS PHY context is present, every
 * such mutation must go through its queued ADI command path. Host-owned
 * tuning has no NIOS PHY and retains the raw-register interface. */
static inline bool rfic_direct_ad9361_write_allowed(
    bool faulted, bool device_owned, bool nios_phy_present)
{
    return rfic_direct_mutation_allowed(faulted, device_owned) &&
           !nios_phy_present;
}

static inline bool rfic_direct_gain_table_write_allowed(
    bool faulted, bool device_owned, bool nios_phy_present)
{
    return rfic_direct_mutation_allowed(faulted, device_owned) &&
           !nios_phy_present;
}

static inline bool rfic_direct_ad9361_batch_allowed(
    bool faulted, bool device_owned, bool nios_phy_present,
    uint16_t count, uint64_t packed_entries)
{
    (void)packed_entries;
    if (count == 0 || count > 3) {
        return false;
    }

    return rfic_direct_ad9361_write_allowed(
        faulted, device_owned, nios_phy_present);
}

static inline void rfic_rfdc_calibration_history_reset(
    uint64_t history[RFIC_RX_RF_PORT_COUNT])
{
    uint32_t port;

    if (history != 0) {
        for (port = 0; port < RFIC_RX_RF_PORT_COUNT; port++) {
            history[port] = 0;
        }
    }
}

static inline bool rfic_rfdc_calibration_required(uint64_t last_hz,
                                                  uint64_t requested_hz,
                                                  uint64_t threshold_hz)
{
    uint64_t delta_hz = requested_hz > last_hz
                            ? requested_hz - last_hz
                            : last_hz - requested_hz;

    return last_hz == 0 || delta_hz > threshold_hz;
}

/* UG-570's RFDC result is selected by the active RX RF input. A frequency
 * cache without the RF-port dimension can incorrectly reuse A-port
 * calibration for B/C (or vice versa), especially at the 3 GHz switch where
 * the LO delta can be tiny. The caller indexes history by selected port. */
static inline bool rfic_rfdc_port_calibration_required(
    const uint64_t history[RFIC_RX_RF_PORT_COUNT], uint32_t rf_port,
    uint64_t requested_hz, uint64_t threshold_hz)
{
    if (history == 0 || rf_port >= RFIC_RX_RF_PORT_COUNT) {
        return true;
    }

    return rfic_rfdc_calibration_required(history[rf_port], requested_hz,
                                          threshold_hz);
}

#endif
