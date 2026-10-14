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

#ifndef BLADERF_NIOS_DEVICES_RFIC_H_
#define BLADERF_NIOS_DEVICES_RFIC_H_

#include <stdbool.h>
#include <stdint.h>

#include "bladerf2_common.h"
#include "debug.h"
#include "devices.h"

#include "devices_rfic_cmds.h"
#include "devices_rfic_queue.h"
#include "rfic_channel_validation.h"
#include "rfic_calibration_policy.h"
#include <ad9361.h>
#include <ad9361_api.h>


/******************************************************************************/
/* Types */
/******************************************************************************/

typedef uint8_t command_bitmask;

enum rfic_init_tail_phase {
    RFIC_INIT_TAIL_IDLE = 0,
    RFIC_INIT_TAIL_RESET_LO, /* Reserved progress code; cold INIT skips it. */
    RFIC_INIT_TAIL_FILTER,
    RFIC_INIT_TAIL_SAMPLE_RATE,
    RFIC_INIT_TAIL_FINAL_LO,
    RFIC_INIT_TAIL_TX_GAIN,
    RFIC_INIT_TAIL_TX_MUTE,
    RFIC_INIT_TAIL_DONE,
};

#define RFIC_CMD_INIT_REQD 1   /**< Command requires initialized state */
#define RFIC_CMD_CHAN_SYSTEM 2 /**< Command valid for wildcard channels */
#define RFIC_CMD_CHAN_TX 4     /**< Command valid for TX channels */
#define RFIC_CMD_CHAN_RX 8     /**< Command valid for RX channels */

/**
 * RFIC command handler state structure
 */
struct rfic_state {
    /* Initialization state */
    bladerf_rfic_init_state init_state;

    /* Last stage _rfic_initialize entered (BLADERF_RFIC_INIT_STAGE_*),
     * reported through the STATUS command so a failed init can say where it
     * stopped instead of only "false". */
    uint8_t init_stage;

    /* Cold INIT retains the ADI candidate-PHY constructor while the sole
     * RFIC queue worker yields between setup/calibration polls. The context
     * and init parameters remain resident until terminal result/cleanup. */
    struct ad9361_init_transaction init_job;
    bool init_job_started;
    bool init_job_pending;
    int32_t init_job_status;
    enum rfic_init_tail_phase init_tail_phase;
    uint8_t init_module_index;
    uint8_t init_channel_index;
    bool init_restore_standby;
    bool init_nested_complete;
    int32_t init_nested_status;
    bool init_tail_pending;

    /* NIOS LO programming uses the retained ADI RFPLL clock transaction.
     * The command resumes after PLL lock to run RFDC/TXQ and publish the LO
     * cache; it must not replay port selection or the LO programming phase. */
    struct ad9361_rfpll_clock_transaction rfpll_job;
    bool rfpll_job_pending;
    bool frequency_rfpll_resume_command;
    bladerf_channel frequency_rfpll_channel;
    bladerf_frequency frequency_rfpll_target;
    bool frequency_rfpll_init_sequence;
    int32_t frequency_rfpll_status;

    /* AD9361 state structure */
    struct ad9361_rf_phy *phy;

    /* Set once a calibration has started but completion could not be
     * established. Reject all RFIC traffic until INIT/ON performs RESETB
     * and a complete AD9361 reinitialization succeeds. */
    bool calibration_faulted;

    /* Queue for handling long-running write operations */
    struct rfic_queue write_queue;

    /* RX and TX FIR filter memory */
    bladerf_rfic_rxfir rxfir;
    bladerf_rfic_txfir txfir;

    /* Frequency retrieval is invalid due to fastlock shenanigans */
    bool frequency_invalid[NUM_MODULES];

    /* A failed/partial clock or FIR update makes cached configuration
     * readback ambiguous. Keep getters fail-closed until a full setter
     * operation commits successfully. */
    bool samplerate_invalid[NUM_MODULES];
    /* ADI bandwidth updates program RX and TX filter chains together. */
    bool bandwidth_invalid[NUM_MODULES];
    bool filter_invalid[NUM_MODULES];

    /* RX/TX sample-rate changes share the AD9361 clock tree and bandwidth
     * calibration. Keep the entire clock→filter→TXQ/recovery transaction
     * resident while the RFIC queue yields between TXQ polls. */
    struct ad9361_sample_rate_transaction sample_rate_job;
    bool sample_rate_job_pending;
    bool sample_rate_resume_command;
    bool sample_rate_saved_samplerate_invalid[NUM_MODULES];
    bool sample_rate_saved_bandwidth_invalid[NUM_MODULES];

    /* Standalone RX/TX bandwidth writes also run shared BBF/TXQ work. Keep
     * their ADI transaction resident while the queue yields between polls. */
    struct ad9361_rf_bandwidth_transaction bandwidth_job;
    bool bandwidth_job_pending;
    bool bandwidth_resume_command;

    /* FIR configure/enable uses a retained ADI transaction because enable
     * validation nests clock-chain, analog-filter, and TXQ work. Preserve the
     * command identity so a resumed callback commits cache only, never reloads
     * coefficients or restarts calibration. */
    struct ad9361_fir_enable_transaction fir_enable_job;
    bool fir_enable_job_pending;
    bool fir_enable_resume_command;
    bool fir_enable_init_pair_resume;
    bladerf_channel fir_enable_channel;
    uint32_t fir_enable_filter;
    bladerf_rfic_rxfir fir_enable_init_rx_filter;
    bladerf_rfic_txfir fir_enable_init_tx_filter;

    /* RFDC calibration history is per selected RX RF input. AD9361 keeps
     * separate RFDC values for input A and inputs B/C, so frequency-only
     * history can reuse a correction from the wrong input after a port
     * switch (including a small LO step at the bladeRF 3 GHz crossover). */
    bladerf_frequency last_rfdc_calib_freq[NUM_MODULES]
                                               [RFIC_RX_RF_PORT_COUNT];

    /* Runtime RX retune may yield while RF DC calibration is active. This
     * context stays resident until the hardware completion and ADI restore
     * phase have both reached a terminal result. */
    struct ad9361_rfdc_calibration_job rfdc_job;
    bool rfdc_job_pending;
    bool rfdc_commit_rx_frequency;
    uint8_t rfdc_job_port;
    bladerf_frequency rfdc_job_frequency;
    /* RX enable must defer its RFFE commit until a newly selected RF input's
     * RFDC calibration and tracking/ENSM restoration have completed. */
    bool rfdc_resume_enable_command;
    bladerf_channel rfdc_resume_enable_channel;
    uint32_t rfdc_resume_enable_value;
    uint32_t rfdc_resume_enable_rffe_reg;
    int32_t rfdc_resume_enable_status;

    /* Runtime TX LO retune may yield across TXQ attempts/phase search. The
     * target TX frequency remains invalid until tracking/ENSM restoration and
     * calibration-history commit both complete. */
    struct ad9361_tx_quad_calibration_transaction tx_quad_job;
    bool tx_quad_job_pending;
    bool tx_quad_commit_tx_frequency;
    bool tx_quad_retune_owner;
    /* TX enable resumes its queue callback only after TXQ has restored the
     * RFIC transaction state; the RFFE enable commit must remain after QEC. */
    bool tx_quad_resume_command;
    bladerf_channel tx_quad_resume_channel;
    uint32_t tx_quad_resume_value;
    uint32_t tx_quad_resume_rffe_reg;

    /* TX mute state at standby */
    bool tx_mute_state[2];
};


/******************************************************************************/
/* Function declarations */
/******************************************************************************/

/**
 * RFIC command read (immediate)
 *
 * @param   command     Command to execute
 * @param   channel     Channel to apply command to
 * @param   data        Response from command
 *
 * @return bool (true = success)
 */
bool rfic_command_read_immed(bladerf_rfic_command command,
                             bladerf_channel channel,
                             uint64_t *data);

/**
 * RFIC command write (immediate)
 *
 * @param   command     Command to execute
 * @param   channel     Channel to apply command to
 * @param   data        Argument for command
 *
 * @return bool (true = success)
 */
bool rfic_command_write_immed(bladerf_rfic_command command,
                              bladerf_channel channel,
                              uint64_t data);

/**
 * Invalidate the LO frequency state for a given module
 *
 * If anything affecting the tuning frequency of the RFIC is performed
 * without using the ad9361_* functions, call this function to indicate
 * that the state is invalid. Attempts to get the LO frequency will fail,
 * until the LO frequency is set with a BLADERF_RFIC_COMMAND_FREQUENCY
 * command.
 *
 * @param[in]  module  The module
 */
void rfic_invalidate_frequency(bladerf_module module);
bool rfic_read_frequency_live(bladerf_module module, uint8_t profile,
                              uint64_t *frequency_hz,
                              uint8_t *failure_stage,
                              bool *txq_pending);
bool rfic_disable_tx_output(void);
bool rfic_start_tx_quad_calibration_for_frequency(bool *pending,
                                                   uint8_t *failure_stage);
bool rfic_start_tx_quad_calibration_for_command(bool *pending,
                                                 uint8_t *failure_stage);
bool rfic_start_tx_quad_calibration_for_retune(bool *pending,
                                               uint8_t *failure_stage);
int32_t rfic_poll_tx_quad_calibration_for_retune(uint8_t *failure_stage);
bool rfic_tx_quad_retune_owner(void);
void rfic_fail_tx_quad_calibration(int32_t status,
                                   uint8_t *failure_stage);
bool rfic_frequency_state_available(void);
bool rfic_phy_present(void);
bool rfic_command_state_faulted(void);

/* Check that the peer LO needed for dependent-FDD VCO selection is known. */
bool rfic_frequency_peer_unknown(bladerf_module target);


/******************************************************************************/
/* Helpers */
/******************************************************************************/

/**
 * @brief      Translate a bladerf_direction and channel number to a
 *             bladerf_channel
 *
 * @param      _dir  Direction
 * @param      _idx  Channel number
 *
 * @return     BLADERF_CHANNEL_TX(_idx) or BLADERF_CHANNEL_RX(_idx)
 */
#define CHANNEL_IDX(_dir, _idx) \
    (BLADERF_TX == _dir) ? BLADERF_CHANNEL_TX(_idx) : BLADERF_CHANNEL_RX(_idx)

/**
 * @brief      Iterate over all bladerf_directions
 *
 * @param      _dir  Direction
 */
#define FOR_EACH_DIRECTION(_dir) \
    for (_dir = BLADERF_RX; _dir <= BLADERF_TX; ++_dir)

/**
 * @brief      Iterate over all channels in a given direction
 *
 * @param      _dir      Direction
 * @param      _count    Number of channels
 * @param[out] _index    Index variable (size_t)
 * @param[out] _channel  Channel variable (bladerf_channel)
 *
 * @return     { description_of_the_return_value }
 */
#define FOR_EACH_CHANNEL(_dir, _count, _index, _channel)                    \
    for (_index = 0, _channel = CHANNEL_IDX(_dir, _index); _index < _count; \
         ++_index, _channel   = CHANNEL_IDX(_dir, _index))

/* Packet decoding debugging output */
static char const RFIC_DBG_FMT[] = "%s: cmd=%s ch=%s state=%s [%x]=%x(-%x)\n";
#define RFIC_DBG(prefix, cmd, ch, state, addr, data) \
    DBG(RFIC_DBG_FMT, prefix, cmd, ch, state, addr, data, -data)

/* Error message output */
static char const RFIC_ERR_FMT[] = "%s: error: %s returned status %d\n";
void rfic_command_record_error(int32_t status);
void rfic_command_record_cleanup_error(int32_t status);
#define RFIC_ERR(thing, retval) \
    do { \
        int32_t const _rfic_error_status = (int32_t)(retval); \
        rfic_command_record_error(_rfic_error_status); \
        DBG(RFIC_ERR_FMT, __FUNCTION__, thing, (int)_rfic_error_status); \
    } while (0)

#include "rfic_check.h"

/**
 * @brief      Error-catching wrapper
 *
 * @param      _fn   The function to wrap
 *
 * @return     false on error (_fn() < 0), continues otherwise
 */
static inline bool _rfic_is_initialized(struct rfic_state *state)
{
    return BLADERF_RFIC_INIT_STATE_ON == state->init_state;
}

static inline uint8_t _rfic_unpack_chan(uint16_t addr)
{
    return (addr >> 8) & 0xF;
}

static inline uint8_t _rfic_unpack_cmd(uint16_t addr)
{
    return addr & 0xF;
}

static inline char const *_rfic_cmdstr(uint8_t cmd)
{
    switch (cmd) {
        case BLADERF_RFIC_COMMAND_STATUS:
            return "STATUS  ";

        case BLADERF_RFIC_COMMAND_TRANSACTION_STATUS:
            return "TXNSTAT ";

        case BLADERF_RFIC_COMMAND_PROGRESS:
            return "PROGRESS ";

        case BLADERF_RFIC_COMMAND_RESULT_DETAIL:
            return "RESULTD ";

        case BLADERF_RFIC_COMMAND_INIT:
            return "INIT    ";

        case BLADERF_RFIC_COMMAND_ENABLE:
            return "ENABLE  ";

        case BLADERF_RFIC_COMMAND_SAMPLERATE:
            return "SAMPRATE";

        case BLADERF_RFIC_COMMAND_FREQUENCY:
            return "FREQ    ";

        case BLADERF_RFIC_COMMAND_BANDWIDTH:
            return "BNDWDTH ";

        case BLADERF_RFIC_COMMAND_GAINMODE:
            return "GAINMD  ";

        case BLADERF_RFIC_COMMAND_GAIN:
            return "GAIN    ";

        case BLADERF_RFIC_COMMAND_RSSI:
            return "RSSI    ";

        case BLADERF_RFIC_COMMAND_FILTER:
            return "FILTER  ";

        case BLADERF_RFIC_COMMAND_TXMUTE:
            return "TXMUTE  ";

        case BLADERF_RFIC_COMMAND_FASTLOCK:
            return "FASTLOCK";

        default:
            return "        ";
    }
}

static inline char const *_rfic_chanstr(uint8_t ch)
{
    switch (ch) {
        case BLADERF_CHANNEL_TX(0):
            return "TX1";
        case BLADERF_CHANNEL_TX(1):
            return "TX2";
        case BLADERF_CHANNEL_RX(0):
            return "RX1";
        case BLADERF_CHANNEL_RX(1):
            return "RX2";
        case RFIC_SYSTEM_CHANNEL:
            return "SYS";
        default:
            return "   ";
    }
}

static inline char const *_rfic_statestr(enum rfic_entry_state state)
{
    switch (state) {
        case ENTRY_STATE_INVALID:
            return "BAD";
        case ENTRY_STATE_NEW:
            return "NEW";
        case ENTRY_STATE_RUNNING:
            return "RUN";
        case ENTRY_STATE_COMPLETE:
            return "OK ";
        default:
            return "   ";
    }
}

#endif  // BLADERF_NIOS_DEVICES_RFIC_H_
