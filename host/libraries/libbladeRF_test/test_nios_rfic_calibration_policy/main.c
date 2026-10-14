#include <assert.h>
#include <errno.h>
#include <stdint.h>

#include "rfic_calibration_policy.h"
#include "rfic_cal_done_policy.h"
#include "rfic_scheduler_policy.h"
#include "rfic_tx_output_policy.h"

#define RFDC_THRESHOLD_HZ UINT64_C(100000000)

int main(void)
{
    uint32_t rffe = (1u << 1) | (1u << 2) | (1u << 15) | (1u << 16) |
                    (1u << 18) | (3u << 11) | (2u << 13) | (1u << 5);
    uint32_t tx_control_mask = 1u << 2;
    uint32_t tx_lane_mask = (1u << 16) | (1u << 18);
    uint32_t tx_switch_mask = (3u << 11) | (3u << 13);
    uint32_t disconnected;
    uint8_t ctrl_out_bit;
    uint64_t history[RFIC_RX_RF_PORT_COUNT] = {
        UINT64_C(2400000000), 0, 0
    };
    const uint64_t reinit_frequency_hz = UINT64_C(1835000000);

    assert(rfic_calibration_command_allowed(false, false));
    assert(rfic_calibration_command_allowed(false, true));
    assert(!rfic_calibration_command_allowed(true, false));
    assert(rfic_calibration_command_allowed(true, true));
    assert(rfic_direct_mutation_allowed(false, false));
    assert(!rfic_direct_mutation_allowed(true, false));
    assert(!rfic_direct_mutation_allowed(false, true));
    assert(!rfic_direct_mutation_allowed(true, true));
    /* Raw SPI can invalidate any cached PHY state, not only the CTRL_OUT
     * mapping. NIOS-owned mode must use the queued ADI command interface. */
    assert(!rfic_direct_ad9361_write_allowed(false, false, true));
    assert(rfic_direct_ad9361_write_allowed(false, false, false));
    assert(!rfic_direct_ad9361_write_allowed(true, false, false));
    assert(!rfic_direct_ad9361_write_allowed(false, true, false));
    assert(!rfic_direct_gain_table_write_allowed(false, false, true));
    assert(rfic_direct_gain_table_write_allowed(false, false, false));
    assert(!rfic_direct_gain_table_write_allowed(true, false, false));
    assert(!rfic_direct_gain_table_write_allowed(false, true, false));
    assert(!rfic_direct_ad9361_batch_allowed(
        false, false, true, 2,
        (UINT64_C(0x435) << 18) | (UINT64_C(0x247) << 36)));
    assert(!rfic_direct_ad9361_batch_allowed(
        false, false, true, 3,
        UINT64_C(0x247) | (UINT64_C(0x436) << 18) |
            (UINT64_C(0x247) << 36)));
    assert(!rfic_direct_ad9361_batch_allowed(
        false, false, true, 2,
        UINT64_C(0x247) | (UINT64_C(0x287) << 18)));
    assert(rfic_direct_ad9361_batch_allowed(
        false, false, false, 1, UINT64_C(0x435)));
    assert(!rfic_direct_ad9361_batch_allowed(
        false, false, true, 4, 0));

    /* A side-effect-free sample-rate preflight rejection must preserve the
     * prior readback cache. Active work or an uncertain post-mutation failure
     * invalidates both shared clock/bandwidth readbacks. */
    assert(rfic_sample_rate_cache_action(false, true, true) ==
           RFIC_SAMPLE_RATE_CACHE_PRESERVE);
    assert(rfic_sample_rate_cache_action(false, true, false) ==
           RFIC_SAMPLE_RATE_CACHE_INVALIDATE);
    assert(rfic_sample_rate_cache_action(true, false, true) ==
           RFIC_SAMPLE_RATE_CACHE_INVALIDATE);
    assert(rfic_sample_rate_cache_action(false, false, true) ==
           RFIC_SAMPLE_RATE_CACHE_PRESERVE);
    assert(rfic_sample_rate_restore_cache_after_rollback(true, true, true));
    assert(!rfic_sample_rate_restore_cache_after_rollback(false, true, true));
    assert(!rfic_sample_rate_restore_cache_after_rollback(true, false, true));
    assert(!rfic_sample_rate_restore_cache_after_rollback(true, true, false));

    /* Calibration completion is not command completion when a post-calibration
     * hardware commit remains. A TX enable must resume and commit RFFE only
     * after TXQ has completed; failure must retire as failure. */
    assert(rfic_calibration_poll_action(1, false) ==
           RFIC_CALIBRATION_POLL_AGAIN);
    assert(rfic_calibration_poll_action(0, false) ==
           RFIC_CALIBRATION_POLL_FINISH_COMMAND);
    assert(rfic_calibration_poll_action(0, true) ==
           RFIC_CALIBRATION_POLL_RESUME_COMMAND);
    assert(rfic_calibration_poll_action(-EIO, true) ==
           RFIC_CALIBRATION_POLL_FAIL_COMMAND);
    assert(rfic_rfdc_poll_action(1, true) == RFIC_RFDC_POLL_AGAIN);
    assert(rfic_rfdc_poll_action(0, true) ==
           RFIC_RFDC_POLL_RESUME_RX_ENABLE);
    assert(rfic_rfdc_poll_action(-EIO, true) ==
           RFIC_RFDC_POLL_RESUME_RX_ENABLE);
    assert(rfic_rfdc_poll_action(0, false) == RFIC_RFDC_POLL_COMPLETE);
    assert(rfic_rfdc_poll_action(-EIO, false) == RFIC_RFDC_POLL_COMPLETE);
    assert(rfic_tx_enable_continuation_matches(2, 1, 2, 7));
    assert(!rfic_tx_enable_continuation_matches(2, 1, 3, 1));
    assert(!rfic_tx_enable_continuation_matches(2, 0, 2, 1));
    assert(rfic_rx_enable_resume_commit_allowed(0, true, true));
    assert(!rfic_rx_enable_resume_commit_allowed(1, true, true));
    assert(!rfic_rx_enable_resume_commit_allowed(-EIO, true, true));
    assert(!rfic_rx_enable_resume_commit_allowed(0, false, true));
    assert(!rfic_rx_enable_resume_commit_allowed(0, true, false));

    /* A waiting queue entry is not an active RFIC owner. Ownership spans a
     * RUNNING callback and every yielded calibration poll, then releases at
     * terminal state so scheduled work cannot starve behind queued requests. */
    assert(!rfic_scheduler_device_owned(false, false, false)); /* NEW / idle */
    assert(rfic_scheduler_device_owned(true, false, false));  /* RUNNING */
    assert(!rfic_scheduler_device_owned(false, false, false)); /* COMPLETE */
    assert(rfic_scheduler_device_owned(true, true, false));    /* yielded RFDC */
    assert(rfic_scheduler_device_owned(false, true, false));   /* pending */
    assert(rfic_scheduler_device_owned(false, false, true));   /* PLL owner */
    assert(rfic_scheduler_calibration_pending(false, false, true, false,
                                              false));
    assert(rfic_scheduler_calibration_pending(false, false, false, true,
                                              false));
    assert(rfic_scheduler_calibration_pending(false, false, false, false,
                                              true));
    assert(rfic_scheduler_device_owned(
        false, rfic_scheduler_calibration_pending(false, false, false, true,
                                                   false),
        false));
    assert(rfic_scheduler_device_owned(
        false, rfic_scheduler_calibration_pending(false, false, true, false,
                                                   false),
        false));
    assert(!rfic_scheduler_reference_tune_allowed(false, false));
    assert(rfic_scheduler_reference_tune_allowed(true, false));
    assert(!rfic_scheduler_reference_tune_allowed(true, true));
    assert(!rfic_scheduler_reference_tune_allowed(
        true, rfic_scheduler_device_owned(false, false, true)));
    assert(rfic_scheduler_reference_write_allowed(false));
    assert(!rfic_scheduler_reference_write_allowed(true));

    assert(rfic_cal_done_output_bit(RFIC_AD9361_RFDC_CAL,
                                    &ctrl_out_bit) && ctrl_out_bit == 7);
    assert(rfic_cal_done_output_bit(RFIC_AD9361_BBDC_CAL,
                                    &ctrl_out_bit) && ctrl_out_bit == 7);
    assert(rfic_cal_done_output_bit(RFIC_AD9361_TX_QUAD_CAL,
                                    &ctrl_out_bit) && ctrl_out_bit == 7);
    assert(rfic_cal_done_output_bit(RFIC_AD9361_RX_QUAD_CAL,
                                    &ctrl_out_bit) && ctrl_out_bit == 7);
    assert(rfic_cal_done_output_bit(RFIC_AD9361_RX_GAIN_STEP_CAL,
                                    &ctrl_out_bit) && ctrl_out_bit == 7);
    assert(rfic_cal_done_output_bit(RFIC_AD9361_RX_BB_TUNE_CAL,
                                    &ctrl_out_bit) && ctrl_out_bit == 4);
    assert(rfic_cal_done_output_bit(RFIC_AD9361_TX_BB_TUNE_CAL,
                                    &ctrl_out_bit) && ctrl_out_bit == 3);
    assert(!rfic_cal_done_output_bit(RFIC_AD9361_TXMON_CAL, &ctrl_out_bit));
    assert(!rfic_cal_done_output_bit(RFIC_AD9361_RFDC_CAL |
                                     RFIC_AD9361_BBDC_CAL, &ctrl_out_bit));

    disconnected = rfic_tx_disconnect_value(
        rffe, tx_control_mask, tx_lane_mask, tx_switch_mask);
    assert((disconnected & (tx_control_mask | tx_lane_mask |
                            tx_switch_mask)) == 0);
    assert((disconnected & (1u << 1)) == (rffe & (1u << 1)));
    assert((disconnected & (1u << 15)) == (rffe & (1u << 15)));
    assert((disconnected & (1u << 5)) == (rffe & (1u << 5)));

    /* OFF -> ON creates a new PHY, so old history must not suppress the
     * first calibration even when the requested LO is unchanged. */
    rfic_rfdc_calibration_history_reset(history);
    assert(history[0] == 0 && history[1] == 0 && history[2] == 0);
    assert(rfic_rfdc_calibration_required(
        history[0], reinit_frequency_hz, RFDC_THRESHOLD_HZ));

    /* ADI setup calibrated port A at 2.4 GHz; bladeRF routes 2.4 GHz to B.
     * The cold-init staging tune must not mark B calibrated, and the final
     * operating tune must therefore run RFDC on B despite zero LO delta. */
    history[0] = UINT64_C(2400000000);
    assert(!rfic_rfdc_port_calibration_required(
        history, 1, UINT64_C(70000000), RFDC_THRESHOLD_HZ, true,
        UINT64_C(70000000)));
    assert(rfic_rfdc_port_calibration_required(
        history, 1, UINT64_C(2400000000), RFDC_THRESHOLD_HZ, true,
        UINT64_C(70000000)));

    /* The A/B selector can change at the 3 GHz boundary with only a tiny LO
     * step. Calibration is keyed by port, so this cannot reuse A's result. */
    history[0] = UINT64_C(2999900000);
    history[1] = 0;
    assert(rfic_rfdc_port_calibration_required(
        history, 1, UINT64_C(3000100000), RFDC_THRESHOLD_HZ, false,
        UINT64_C(70000000)));
    history[1] = UINT64_C(3000100000);
    assert(!rfic_rfdc_port_calibration_required(
        history, 1, UINT64_C(3050000000), RFDC_THRESHOLD_HZ, false,
        UINT64_C(70000000)));
    assert(rfic_rfdc_port_calibration_required(
        history, 1, UINT64_C(3100100000), RFDC_THRESHOLD_HZ, false,
        UINT64_C(70000000)));

    assert(!rfic_rfdc_calibration_required(
        reinit_frequency_hz, reinit_frequency_hz + RFDC_THRESHOLD_HZ - 1,
        RFDC_THRESHOLD_HZ));
    assert(rfic_rfdc_calibration_required(
        reinit_frequency_hz, reinit_frequency_hz + RFDC_THRESHOLD_HZ,
        RFDC_THRESHOLD_HZ));
    return 0;
}
