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

#ifndef BLADERF2_COMMON_H_
#define BLADERF2_COMMON_H_

#if !defined(BLADERF_NIOS_BUILD) && !defined(BLADERF_NIOS_PC_SIMULATION)
#include "log.h"
#endif

#include "bladerf2_common.h"
#include "helpers/version.h"
#include "streaming/sync.h"
#include "thread.h"


/******************************************************************************/
/* Types */
/******************************************************************************/

enum bladerf2_vctcxo_trim_source {
    TRIM_SOURCE_NONE,
    TRIM_SOURCE_TRIM_DAC,
    TRIM_SOURCE_PLL,
    TRIM_SOURCE_AUXDAC
};

enum bladerf2_rfic_command_mode {
    RFIC_COMMAND_HOST, /**< Host-based control */
    RFIC_COMMAND_FPGA, /**< FPGA-based control */
};

struct controller_fns {
    bool (*is_present)(struct bladerf *dev);
    bool (*is_initialized)(struct bladerf *dev);
    bool (*is_standby)(struct bladerf *dev);
    int (*get_init_state)(struct bladerf *dev, bladerf_rfic_init_state *state);

    int (*initialize)(struct bladerf *dev);
    int (*standby)(struct bladerf *dev);
    int (*deinitialize)(struct bladerf *dev);

    int (*enable_module)(struct bladerf *dev, bladerf_channel ch, bool enable);

    int (*get_sample_rate)(struct bladerf *dev,
                           bladerf_channel ch,
                           bladerf_sample_rate *rate);
    int (*set_sample_rate)(struct bladerf *dev,
                           bladerf_channel ch,
                           bladerf_sample_rate rate);

    int (*get_frequency)(struct bladerf *dev,
                         bladerf_channel ch,
                         bladerf_frequency *frequency);
    int (*set_frequency)(struct bladerf *dev,
                         bladerf_channel ch,
                         bladerf_frequency frequency);
    int (*select_band)(struct bladerf *dev,
                       bladerf_channel ch,
                       bladerf_frequency frequency);

    int (*get_bandwidth)(struct bladerf *dev,
                         bladerf_channel ch,
                         bladerf_bandwidth *bandwidth);
    int (*set_bandwidth)(struct bladerf *dev,
                         bladerf_channel ch,
                         bladerf_bandwidth bandwidth,
                         bladerf_bandwidth *actual);

    int (*get_gain_mode)(struct bladerf *dev,
                         bladerf_channel ch,
                         bladerf_gain_mode *mode);
    int (*set_gain_mode)(struct bladerf *dev,
                         bladerf_channel ch,
                         bladerf_gain_mode mode);

    int (*get_gain)(struct bladerf *dev, bladerf_channel ch, int *gain);
    int (*set_gain)(struct bladerf *dev, bladerf_channel ch, int gain);

    int (*get_gain_stage)(struct bladerf *dev,
                          bladerf_channel ch,
                          char const *stage,
                          int *gain);
    int (*set_gain_stage)(struct bladerf *dev,
                          bladerf_channel ch,
                          char const *stage,
                          int gain);

    int (*get_rssi)(struct bladerf *dev,
                    bladerf_channel ch,
                    int *pre_rssi,
                    int *sym_rssi);

    int (*get_filter)(struct bladerf *dev,
                      bladerf_channel ch,
                      bladerf_rfic_rxfir *rxfir,
                      bladerf_rfic_txfir *txfir);
    int (*set_filter)(struct bladerf *dev,
                      bladerf_channel ch,
                      bladerf_rfic_rxfir rxfir,
                      bladerf_rfic_txfir txfir);

    int (*get_txmute)(struct bladerf *dev, bladerf_channel ch, bool *state);
    int (*set_txmute)(struct bladerf *dev, bladerf_channel ch, bool state);

    int (*store_fastlock_profile)(struct bladerf *dev,
                                  bladerf_channel ch,
                                  uint32_t profile);

    int (*save_fastlock_profile)(struct bladerf *dev,
                                 bladerf_channel ch,
                                 uint32_t profile,
                                 uint8_t *values);

    enum bladerf2_rfic_command_mode const command_mode;
};

struct bladerf2_board_data {
    /* RF link epoch bookkeeping. The START/STOP toggles are shared by both
     * directions, so STOP may only be sent when the LAST enabled direction
     * goes down -- disabling TX while RX streams must not end RX's epoch.
     * Index 0 = RX, 1 = TX. */
    bool rf_link_dir_on[2];

    /* Board state */
    enum {
        STATE_UNINITIALIZED,
        STATE_FIRMWARE_LOADED,
        STATE_FPGA_LOADED,
        STATE_INITIALIZED,
    } state;

    /* AD9361 PHY Handle */
    struct ad9361_rf_phy *phy;

    /* RFIC configuration parameters */
    void *rfic_init_params;

    /* Bitmask of capabilities determined by version numbers */
    uint64_t capabilities;

    /* Format currently being used with a module, or -1 if module is not used */
    bladerf_format module_format[NUM_MODULES];

    /* Which mode of operation we use for tuning */
    bladerf_tuning_mode tuning_mode;

    /* Board properties */
    bladerf_fpga_size fpga_size;
    /* Data message size */
    size_t msg_size;

    /* Version information */
    struct bladerf_version fpga_version;
    struct bladerf_version fw_version;
    char fpga_version_str[BLADERF_VERSION_STR_MAX + 1];
    char fw_version_str[BLADERF_VERSION_STR_MAX + 1];

    /* Synchronous interface handles */
    struct bladerf_sync sync[2];

    /* Number of running RX async streams whose format cannot carry an FPGA
     * epoch tag. Guarded by dev->lock. Epoch-required transitions must not
     * retune underneath one of these legacy consumers. */
    unsigned int rx_async_epochless_stream_count;
    /* Number of active RX_X1 async streams; guarded by dev->lock. */
    unsigned int rx_async_x1_stream_count;

    /* VCTCXO trim state */
    enum bladerf2_vctcxo_trim_source trim_source;
    uint16_t trimdac_last_value;   /**< saved running value */
    uint16_t trimdac_stored_value; /**< cached value read from SPI flash */

    /* Quick Tune Profile Status */
    uint16_t quick_tune_tx_profile;
    uint16_t quick_tune_rx_profile;

    /* RFIC backend command handling */
    struct controller_fns const *rfic;

    /* RFIC FIR Filter status */
    bladerf_rfic_rxfir rxfir;
    bladerf_rfic_txfir txfir;

    /* If true, RFIC control will be fully de-initialized on close, instead of
     * just put into a standby state. */
    bool rfic_reset_on_close;

    /* Poll the sticky FPGA RX-fault aggregate outside libusb completion
     * callbacks so an active certified stream can be invalidated with its
     * hardware cause before endpoint timeout delivery. */
    THREAD rx_fault_monitor_thread;
    MUTEX rx_fault_monitor_lock;
    COND rx_fault_monitor_cond;
    bool rx_fault_monitor_sync_initialized;
    bool rx_fault_monitor_started;
    bool rx_fault_monitor_stop;
    uint64_t rx_fpga_loss_count_last;
    bool rx_fpga_loss_count_valid;

    /* ADR-0207: bounded chronological history of host-observed RX
     * transition events. Event readers must not take dev->lock because the
     * async USB callback dispatches events while setters may hold that lock. */
#define BLADERF2_RF_EVENT_HISTORY_SIZE BLADERF_RF_EVENT_HISTORY_SIZE
    MUTEX rf_transition_event_lock;
    bool rf_transition_event_lock_initialized;
    struct bladerf_rf_event rf_transition_events[
        BLADERF2_RF_EVENT_HISTORY_SIZE];
    uint64_t rf_transition_event_sequences[
        BLADERF2_RF_EVENT_HISTORY_SIZE];
    uint32_t rf_transition_event_head;
    uint32_t rf_transition_event_count;
    uint64_t rf_transition_event_sequence;

    /* NIOS time-tamer duration is separate from host monotonic event times.
     * Keep it by transaction without changing the public event struct ABI. */
    struct {
        uint32_t transaction_id;
        uint64_t duration_ticks;
        bool nios_retune_observed;
        bool duration_valid;
    } rf_transition_nios_timing[BLADERF2_RF_EVENT_HISTORY_SIZE];

    /* Latest event is retained for the synchronous wait API. */
    uint32_t rf_transition_next_id;
    uint32_t rf_transition_current_id;
    bladerf_channel rf_transition_current_channel;
    uint8_t rx_channel_enable_mask;
    bool rx_channel_enable_mask_valid;
    uint32_t rf_transition_required_events_mask;
    uint8_t rf_transition_epoch_id;
    bool rx_epoch_snapshot_capability_checked;
    bool rx_epoch_snapshot_supported;
    /* Async USB callback threads must snapshot epoch policy without taking
     * dev->lock: host setters hold dev->lock while waiting on USB control I/O. */
    MUTEX rx_async_epoch_lock;
    bool rx_async_epoch_lock_initialized;
    COND rx_async_epoch_cond;
    bool rx_async_epoch_cond_initialized;
    uint64_t rf_transition_requested_frequency_hz;
    uint64_t rf_transition_readback_frequency_hz;
    bladerf_rf_state rf_transition_state;
    struct bladerf_rf_event rf_transition_last_event;
    bool rf_transition_epoch_contract_enabled;
    bool rf_transition_first_host_data_required;
    bool rf_transition_rx_x2_host_data_required;
    uint32_t rf_transition_rx_x2_host_data_transaction_id;
    uint64_t rf_transition_first_host_data_deadline_ns;
    bool rf_transition_epoch_certified;
#ifdef BLADERF_ENABLE_TEST_RX_EPOCH_METADATA_FAULT_INJECTION
    bool test_rx_epoch_metadata_fault_injected;
#endif
    bool rx_format_unsupported_reported;
    bool rx_async_data_withheld_reported;
    /* Set while the current epoch has an unannounced/active invalid data
     * interval. Cleared only after a host-validated packet is published. */
    bool rx_async_data_withheld_active;
    bool rx_async_timestamp_discontinuity_reported;
    bool rx_async_have_expected_timestamp;
    uint8_t rx_async_timestamp_epoch_id;
    uint64_t rx_async_expected_timestamp;
    uint8_t rf_transition_certified_epoch_id;
    uint64_t rf_transition_first_valid_timestamp;
    struct bladerf_rf_event rf_transition_certified_epoch_event;
    struct bladerf_rf_event rf_transition_first_host_data_event;
    bool rf_transition_first_host_data_reported;
    bool rf_transition_pending;
    bool rf_transition_waiting;
    bool rf_transition_spi_trace_enabled;
    bool rf_transition_setter_active;
    bool rf_transition_scheduling_quick_tune;
    uint64_t rf_transition_spi_first_write_ns;
    uint64_t rf_transition_spi_last_write_ns;
    uint32_t rf_transition_spi_write_count;
    int rf_transition_spi_last_status;
#ifdef BLADERF_ENABLE_TEST_SPI_FAULT_INJECTION
    uint32_t rf_transition_test_fault_transaction_id;
    uint32_t rf_transition_test_fault_write_ordinal;
    bool rf_transition_test_fault_consumed;
#endif
};

/* Internal sync-RX hook: record when the first post-epoch META samples pass
 * host validation. Optional RF transition waits may require this event before
 * reporting success; the event itself never certifies the FPGA epoch. */
void bladerf2_rx_transition_note_first_packet(
    struct bladerf *dev, const struct bladerf_metadata *metadata,
    bladerf_channel_layout layout);
/* Caller holds rx_async_epoch_lock; lets async admission and host-data event
 * publication share one linearization point without taking dev->lock. */
void bladerf2_rx_transition_note_first_packet_epoch_locked(
    struct bladerf *dev, const struct bladerf_metadata *metadata,
    bladerf_channel_layout layout);
int bladerf2_rx_data_invalidate(struct bladerf *dev, bladerf_channel ch,
                                uint32_t reason);
void bladerf2_rx_reconfigure_complete(struct bladerf *dev,
                                      bladerf_channel ch);
void bladerf2_rx_stream_overrun(struct bladerf *dev, uint32_t source_flags);
/* Lock-safe publication for the sync worker callback context. */
void bladerf2_rx_worker_stream_overrun(struct bladerf *dev,
                                       uint32_t source_flags);
void bladerf2_rx_fpga_loss(struct bladerf *dev, uint8_t epoch_id,
                           uint64_t loss_count);
void bladerf2_rx_async_stream_overrun(struct bladerf *dev);
int bladerf2_rx_fault_monitor_start(struct bladerf *dev);
void bladerf2_rx_fault_monitor_stop(struct bladerf *dev);
void bladerf2_rf_event_append(struct bladerf2_board_data *board_data,
                              const struct bladerf_rf_event *event);
void bladerf2_rf_event_append_locked(
    struct bladerf2_board_data *board_data,
    const struct bladerf_rf_event *event);
void bladerf2_rx_format_unsupported(struct bladerf *dev,
                                    bladerf_format format,
                                    bool deduplicate);
void bladerf2_rx_layout_unsupported(struct bladerf *dev,
                                    bladerf_channel_layout layout,
                                    bool active_requirement_context);
void bladerf2_rx_data_withheld(struct bladerf *dev, uint32_t reason);
void bladerf2_rx_data_withheld_at(struct bladerf *dev, uint32_t reason,
                                  uint8_t epoch_id,
                                  uint64_t fpga_timestamp,
                                  bool fpga_timestamp_valid);
void bladerf2_rx_data_withheld_reset(struct bladerf *dev);
/* Caller holds rx_async_epoch_lock; a certified data packet ended a withheld
 * interval, so subsequent faults must be published as a new interval. */
void bladerf2_rx_data_rearm_notifications_locked(
    struct bladerf2_board_data *board_data);
/* Caller holds rx_async_epoch_lock. Snapshot-backed event publication must
 * not depend on the bounded history ring retaining RX_EPOCH_VALID. */
void bladerf2_rx_data_note_first_packet_locked(
    struct bladerf2_board_data *board_data,
    const struct bladerf_metadata *metadata,
    bladerf_channel_layout layout);
void bladerf2_rx_async_timestamp_discontinuity(
    struct bladerf *dev, uint8_t expected_epoch_id,
    uint64_t first_unvalidated_timestamp);
void bladerf2_rx_transition_spi_observe(struct bladerf *dev, bool begin,
                                        int status);
void bladerf2_rx_transition_spi_observe_rollback(struct bladerf *dev,
                                                 uint32_t write_count);
#ifdef BLADERF_ENABLE_TEST_SPI_FAULT_INJECTION
int bladerf2_rx_transition_spi_test_should_fail(struct bladerf *dev);
#endif

struct bladerf_rfic_status_register {
    bool rfic_initialized;
    size_t write_queue_length;
    /* Result of the last command the NIOS write queue retired. The firmware
     * has always reported it (devices_rfic_cmds.c:290-301, WQSUCCESS bit);
     * the host used to discard it, so a queued command that drained and
     * failed looked exactly like one that succeeded. */
    bool last_write_success;
};


/******************************************************************************/
/* Externs */
/******************************************************************************/

extern AD9361_InitParam bladerf2_rfic_init_params;
extern AD9361_InitParam bladerf2_rfic_init_params_fastagc_burst;
extern AD9361_RXFIRConfig bladerf2_rfic_rx_fir_config;
extern AD9361_TXFIRConfig bladerf2_rfic_tx_fir_config;
extern AD9361_RXFIRConfig bladerf2_rfic_rx_fir_config_dec2;
extern AD9361_TXFIRConfig bladerf2_rfic_tx_fir_config_int2;
extern AD9361_RXFIRConfig bladerf2_rfic_rx_fir_config_dec4;
extern AD9361_TXFIRConfig bladerf2_rfic_tx_fir_config_int4;
extern const float ina219_r_shunt;


/******************************************************************************/
/* Constants */
/******************************************************************************/

extern char const *bladerf2_state_to_string[4];


/******************************************************************************/
/* Macros */
/******************************************************************************/

/* Macro for logging and returning an error status. This should be used for
 * errors defined in the \ref RETCODES list. */
#define RETURN_ERROR_STATUS(_what, _status)                   \
    do {                                                      \
        log_error("%s: %s failed: %s\n", __FUNCTION__, _what, \
                  bladerf_strerror(_status));                 \
        return _status;                                       \
    } while (0)

/* Macro for converting, logging, and returning libad9361 error codes. */
#define RETURN_ERROR_AD9361(_what, _status)                           \
    do {                                                              \
        RETURN_ERROR_STATUS(_what, errno_ad9361_to_bladerf(_status)); \
    } while (0)

/* Macro for logging and returning ::BLADERF_ERR_INVAL */
#define RETURN_INVAL_ARG(_what, _arg, _why)                               \
    do {                                                                  \
        log_error("%s: %s '%s' invalid: %s\n", __FUNCTION__, _what, #_arg, \
                  _why);                                                  \
        return BLADERF_ERR_INVAL;                                         \
    } while (0)

#define RETURN_INVAL(_what, _why)                                     \
    do {                                                              \
        log_error("%s: %s invalid: %s\n", __FUNCTION__, _what, _why); \
        return BLADERF_ERR_INVAL;                                     \
    } while (0)

/**
 * @brief   Null test for multiple variables
 *
 * @param   ...  The variables to check
 *
 * @return  RETURN_INVAL if any var is null, continues otherwise
 */
#define NULL_CHECK(...) \
    do { \
        void *_ptrs[] = { (void*)__VA_ARGS__ }; \
        const char *_names[] = { #__VA_ARGS__ }; \
        for (size_t _i = 0; _i < sizeof(_ptrs)/sizeof(_ptrs[0]); _i++) { \
            if (NULL == _ptrs[_i]) { \
                RETURN_INVAL(_names[_i], "is null"); \
            } \
        } \
    } while (0)

/**
 * @brief   Null test, with mutex unlock on failure
 *
 * @param   _var  The variable to check
 *
 * @return  RETURN_INVAL if _var is null, continues otherwise
 */
#define NULL_CHECK_LOCKED(_var)             \
    do {                                    \
        NULL_CHECK(dev);                    \
                                            \
        if (NULL == _var) {                 \
            MUTEX_UNLOCK(__lock);           \
            RETURN_INVAL(#_var, "is null"); \
        }                                   \
    } while (0)

/**
 * @brief   Board state check
 *
 * @param   _state  Minimum sufficient board state
 *
 * @return  BLADERF_ERR_NOT_INIT if board's state is less than _state, continues
 *          otherwise
 */
#define CHECK_BOARD_STATE(_state)                                         \
    do {                                                                  \
        NULL_CHECK(dev);                                                  \
        NULL_CHECK(dev->board);                                           \
                                                                          \
        struct bladerf2_board_data *_bd = dev->board_data;                \
                                                                          \
        if (_bd->state < _state) {                                        \
            log_error("%s: Board state insufficient for operation "       \
                      "(current \"%s\", requires \"%s\").\n",             \
                      __FUNCTION__, bladerf2_state_to_string[_bd->state], \
                      bladerf2_state_to_string[_state]);                  \
                                                                          \
            return BLADERF_ERR_NOT_INIT;                                  \
        }                                                                 \
    } while (0)

/**
 * @brief   Test if board is a bladeRF 2
 *
 * @param   _dev  Device handle
 *
 * @return  BLADERF_ERR_UNSUPPORTED if board is not a bladeRF 2, continues
 *          otherwise
 */
#define CHECK_BOARD_IS_BLADERF2(_dev)                                        \
    do {                                                                     \
        NULL_CHECK(_dev);                                                    \
        NULL_CHECK(_dev->board);                                             \
                                                                             \
        if (_dev->board != &bladerf2_board_fns) {                            \
            log_error("%s: Board type \"%s\" not supported\n", __FUNCTION__, \
                      _dev->board->name);                                    \
            return BLADERF_ERR_UNSUPPORTED;                                  \
        }                                                                    \
    } while (0)

/**
 * @brief   Call a function and return early if it fails
 *
 * @param   _fn   The function
 *
 * @return  function return value if less than zero; continues otherwise
 */
#define CHECK_STATUS(_fn)                  \
    do {                                   \
        int _s = _fn;                      \
        if (_s < 0) {                      \
            RETURN_ERROR_STATUS(#_fn, _s); \
        }                                  \
    } while (0)

/**
 * @brief   Call a function and goto error if it fails
 *
 * @param   _fn   The function
 *
 * @note    `int status` must be declared in the including scope
 */
#define CHECK_STATUS_GOTO(_fn)                             \
    do {                                                   \
        status = _fn;                                      \
        if (status < 0) {                                  \
            log_error("%s: %i failed: %s\n", #_fn, status, \
                      bladerf_strerror(status));           \
            goto error;                                    \
        }                                                  \
    } while (0)

/**
 * @brief   Call a function and, if it fails, unlock the mutex and return
 *
 * @param   _fn   The function
 *
 * @return  function return value if less than zero; continues otherwise
 */
#define CHECK_STATUS_LOCKED(_fn)           \
    do {                                   \
        int _s = _fn;                      \
        if (_s < 0) {                      \
            MUTEX_UNLOCK(__lock);          \
            RETURN_ERROR_STATUS(#_fn, _s); \
        }                                  \
    } while (0)

/**
 * @brief   Call a function and return early if it fails, with error translation
 *          from AD936x to bladeRF return codes
 *
 * @param   _fn   The function
 *
 * @return  function return value if less than zero; continues otherwise
 */
#define CHECK_AD936X(_fn)                  \
    do {                                   \
        int _s = _fn;                      \
        if (_s < 0) {                      \
            RETURN_ERROR_AD9361(#_fn, _s); \
        }                                  \
    } while (0)

/**
 * @brief   Call a function and, if it fails, unlock the mutex and return, with
 *          error translation from AD936x to bladeRF return codes
 *
 * @param   _fn   The function
 *
 * @return  function return value if less than zero; continues otherwise
 */
#define CHECK_AD936X_LOCKED(_fn)           \
    do {                                   \
        int _s = _fn;                      \
        if (_s < 0) {                      \
            MUTEX_UNLOCK(__lock);          \
            RETURN_ERROR_AD9361(#_fn, _s); \
        }                                  \
    } while (0)

/**
 * @brief   Execute a command block with a mutex lock
 *
 * @note    Variables declared within the including scope must be declared
 *          one-per-line.
 *
 * @param   _lock    Lock to hold
 * @param   _thing   Block to execute
 */
#define WITH_MUTEX(_lock, _thing) \
    do {                          \
        MUTEX *__lock = _lock;    \
                                  \
        MUTEX_LOCK(__lock);       \
        _thing;                   \
        MUTEX_UNLOCK(__lock);     \
    } while (0)

/**
 * @brief   Execute command block, conditional to _mode
 *
 * @param   _dev     Device handle
 * @param   _mode    Command mode
 * @param   _thing   Block to do if it happens
 */
#define IF_COMMAND_MODE(_dev, _mode, _thing)               \
    do {                                                   \
        NULL_CHECK(_dev);                                  \
        NULL_CHECK(_dev->board_data);                      \
                                                           \
        struct bladerf2_board_data *bd = _dev->board_data; \
                                                           \
        if (bd->rfic->command_mode == _mode) {             \
            _thing;                                        \
        };                                                 \
    } while (0)


/******************************************************************************/
/* Functions */
/******************************************************************************/

/**
 * Perform the neccessary device configuration for the specified format
 * (e.g., enabling/disabling timestamp support), first checking that the
 * requested format would not conflict with the other stream direction.
 *
 * @param           dev     Device handle
 * @param[in]       dir     Direction that is currently being configured
 * @param[in]       format  Format the channel is being configured for
 *
 * @return 0 on success, BLADERF_ERR_* on failure
 */
int perform_format_config(struct bladerf *dev,
                          bladerf_direction dir,
                          bladerf_format format);

/**
 * Deconfigure and update any state pertaining what a format that a stream
 * direction is no longer using.
 *
 * @param       dev     Device handle
 * @param[in]   dir     Direction that is currently being deconfigured
 *
 * @return 0 on success, BLADERF_ERR_* on failure
 */
int perform_format_deconfig(struct bladerf *dev, bladerf_direction dir);

bool is_valid_fpga_size(struct bladerf *dev,
                        bladerf_fpga_size fpga,
                        size_t len);

bool is_valid_fw_size(size_t len);

bladerf_tuning_mode default_tuning_mode(struct bladerf *dev);

bool check_total_sample_rate(struct bladerf *dev);

bool does_rffe_dir_have_enabled_ch(uint32_t reg, bladerf_direction dir);

int get_gain_offset(struct bladerf *dev, bladerf_channel ch, float *offset);

int bladerf2_rx_epoch_admission_prepare(
    struct bladerf2_board_data *board_data,
    const struct bladerf_rf_event *epoch_event, uint64_t deadline_ns,
    bool *admission_lock_held);
void bladerf2_rx_epoch_admission_finish(
    struct bladerf2_board_data *board_data, bool *admission_lock_held);

#endif  // BLADERF2_COMMON_H_
