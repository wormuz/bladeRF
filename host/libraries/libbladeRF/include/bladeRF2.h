/**
 * @file bladeRF2.h
 *
 * @brief bladeRF2-specific API
 *
 * Copyright (C) 2013-2017 Nuand LLC
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 */
#ifndef BLADERF2_H_
#define BLADERF2_H_

/**
 * @defgroup BLADERF2 bladeRF2-specific API
 *
 * These functions are thread-safe.
 *
 * @{
 */

/**
 * @defgroup FN_BLADERF2_BIAS_TEE Bias Tee Control
 *
 * @{
 */

/**
 * Get current bias tee state
 *
 * @param       dev     Device handle
 * @param[in]   ch      Channel
 * @param[out]  enable  True if bias tee active, false otherwise
 *
 * @return 0 on success, value from \ref RETCODES list on failure
 */
API_EXPORT
int CALL_CONV bladerf_get_bias_tee(struct bladerf *dev,
                                   bladerf_channel ch,
                                   bool *enable);

/**
 * Get current bias tee state
 *
 * @param       dev     Device handle
 * @param[in]   ch      Channel
 * @param[in]   enable  True to activate bias tee, false to deactivate
 *
 * @return 0 on success, value from \ref RETCODES list on failure
 */
API_EXPORT
int CALL_CONV bladerf_set_bias_tee(struct bladerf *dev,
                                   bladerf_channel ch,
                                   bool enable);

/** @} (End of FN_BLADERF2_BIAS_TEE) */

/**
 * @defgroup FN_BLADERF2_LOW_LEVEL Low-level accessors
 *
 * In most cases, higher-level routines should be used. These routines are only
 * intended to support development and testing.
 *
 * Use these routines with great care, and be sure to reference the relevant
 * schematics, data sheets, and source code (i.e., firmware and hdl).
 *
 * Be careful when mixing these calls with higher-level routines that manipulate
 * the same registers/settings.
 *
 * These functions are thread-safe.
 *
 * @{
 */

/**
 * @defgroup FN_BLADERF2_LOW_LEVEL_RFIC RF Integrated Circuit
 *
 * @{
 */

/**
 * Read a RFIC register
 *
 * @param       dev         Device handle
 * @param[in]   address     Register address
 * @param[out]  val         Register value
 *
 * @return 0 on success, value from \ref RETCODES list on failure
 */
API_EXPORT
int CALL_CONV bladerf_get_rfic_register(struct bladerf *dev,
                                        uint16_t address,
                                        uint8_t *val);
/**
 * Write a RFIC register
 *
 * @param       dev         Device handle
 * @param[in]   address     Register address
 * @param[in]   val         Value to write to register
 *
 * @return 0 on success, value from \ref RETCODES list on failure
 */
API_EXPORT
int CALL_CONV bladerf_set_rfic_register(struct bladerf *dev,
                                        uint16_t address,
                                        uint8_t val);

/** Enable or disable AD9361 RX quadrature tracking for one receive lane.
 *
 * The setting is retained by the AD9361 driver and restored by subsequent
 * RF reconfiguration. This invalidates the current RX data epoch.
 */
API_EXPORT
int CALL_CONV bladerf_set_rx_qec_tracking(struct bladerf *dev,
                                         bladerf_channel channel,
                                         bool enable);

/** Read the cached RX QEC tracking enable state for one receive lane.
 * Returns an error if the AD9361 tracking state is not known.
 */
API_EXPORT
int CALL_CONV bladerf_get_rx_qec_tracking(struct bladerf *dev,
                                         bladerf_channel channel,
                                         bool *enable);

/** Set AD9361 RX BB DC, RF DC and per-lane QEC tracking state together.
 * qec_mask bit 0 controls RX1 and bit 1 controls RX2. Disabling BB-DC
 * tracking can leave a lane-dependent DC component in RX IQ samples; raw
 * RMS/level measurements and DC-sensitive signal acquisition may then be
 * misleading. libbladeRF emits a warning when BB-DC tracking is disabled.
 */
API_EXPORT
int CALL_CONV bladerf_set_rx_tracking_config(struct bladerf *dev,
                                            bool bbdc_enable,
                                            bool rfdc_enable,
                                            uint8_t qec_mask);

/** Read the cached AD9361 RX tracking configuration. */
API_EXPORT
int CALL_CONV bladerf_get_rx_tracking_config(struct bladerf *dev,
                                            bool *bbdc_enable,
                                            bool *rfdc_enable,
                                            uint8_t *qec_mask);

/** Read the latest BB-DC tracking I/Q correction words for one RX lane.
 *
 * AD9361 multiplexes the channel words into registers 0x1A2..0x1A5. This
 * function selects the requested lane and toggles the readback latch in
 * register 0x191, then restores the original 0x191 value. The returned
 * words are raw unsigned 15-bit register values. This readback-latch
 * operation does not change the RX signal path or invalidate its epoch.
 */
API_EXPORT
int CALL_CONV bladerf_get_rx_bbdc_tracking_words(struct bladerf *dev,
                                                 bladerf_channel channel,
                                                 uint16_t *i_word,
                                                 uint16_t *q_word);

/** Reversibly bypass controllable AD9361 RX correction and tracking blocks.
 *
 * When enabled, saves the current state, disables BB-DC/RF-DC/QEC tracking,
 * disables phase/gain correction, zeros the active RF-DC words, and zeros
 * the RX QEC/tracking words. The AD9361's static BB-DC calibration words
 * are readback-only and remain applied. Calling with false restores the
 * saved state. This changes RX data and invalidates the active RX epoch.
 * Bypassing BB-DC tracking can leave a lane-dependent DC component in RX IQ
 * samples; libbladeRF emits a warning when bypass is enabled.
 */
API_EXPORT
int CALL_CONV bladerf_set_rx_corrections_bypass(struct bladerf *dev,
                                                bool bypass);

/** AD9361 BIST tone injection point. */
typedef enum {
    BLADERF_BIST_TONE_DISABLE = 0,
    BLADERF_BIST_TONE_TX = 1,
    BLADERF_BIST_TONE_RX = 2,
} bladerf_bist_tone_mode;

/** Inject an AD9361 BIST tone into the selected RFIC path.
 *
 * For RX injection, channel_mask bits 0..3 mask RX1-I, RX1-Q, RX2-I and
 * RX2-Q respectively; zero enables the tone on both receive channels.
 * Frequency values 0..3 select Fsample/32, Fsample/16, 3*Fsample/32 and
 * Fsample/8; larger values are requested frequencies in Hz. Level is 0,
 * 6, 12 or 18 dBFS attenuation. The change invalidates the RX data epoch.
 */
API_EXPORT
int CALL_CONV bladerf_set_bist_tone(struct bladerf *dev,
                                    bladerf_bist_tone_mode mode,
                                    uint32_t frequency_hz,
                                    uint32_t level_db,
                                    uint8_t channel_mask);

/**
 * Read the RFFE control register
 *
 * This register lives in the FPGA, not in the RFIC, and carries the RF
 * front-end state that no RFIC register reflects: the SPDT switch
 * positions, the per-channel enables, and the direction ENABLE/TXNRX
 * bits. Without it, a caller debugging a dead or attenuated RF path has
 * no way to tell a switch left in its shutdown position from an RFIC
 * problem -- every RFIC register can read back correct while the signal
 * is routed nowhere.
 *
 * The bit positions are the RFFE_CONTROL_* constants in
 * fpga_common/include/bladerf2_common.h.
 *
 * @param       dev         Device handle
 * @param[out]  value       RFFE control register value
 *
 * @return 0 on success, value from \ref RETCODES list on failure
 */
API_EXPORT
int CALL_CONV bladerf_get_rffe_control(struct bladerf *dev, uint32_t *value);

/** Read or set the four FPGA RX ADC IQ-correction enables (RX1 I/Q, RX2 I/Q).
 * The mask uses bits 0..3 in that order. Changing it invalidates RX data.
 */
API_EXPORT
int CALL_CONV bladerf_get_rx_fpga_iqcor_mask(struct bladerf *dev,
                                             uint8_t *mask);

/** Read-only diagnostic: FPGA RX ADC channel registers CHAN_CNTRL, _1 (DC filter), _2 (IQ coeff), _3 for ADC channel 0..3 (RX1 I/Q, RX2 I/Q). */
API_EXPORT
int CALL_CONV bladerf_get_rx_fpga_adc_chan_regs(struct bladerf *dev, uint8_t ch,
                                                uint32_t regs[4]);
API_EXPORT
int CALL_CONV bladerf_set_rx_fpga_iqcor_mask(struct bladerf *dev,
                                             uint8_t mask);

/** Read or set the four FPGA RX ADC DC-filter enables (RX1 I/Q, RX2 I/Q).
 * The mask uses bits 0..3 in that order. Changing it invalidates RX data.
 */
API_EXPORT
int CALL_CONV bladerf_get_rx_fpga_dcfilt_mask(struct bladerf *dev,
                                              uint8_t *mask);
API_EXPORT
int CALL_CONV bladerf_set_rx_fpga_dcfilt_mask(struct bladerf *dev,
                                              uint8_t mask);

/** Read the four RX AXI ADC channel-control words in RX1-I, RX1-Q, RX2-I,
 * RX2-Q order. This is read-only and does not invalidate RX data.
 */
API_EXPORT
int CALL_CONV bladerf_get_rx_fpga_adc_channel_control(
    struct bladerf *dev, uint32_t values[4]);
/** Enable or disable the four RX FPGA ADC output channels (RX1-I, RX1-Q,
 * RX2-I, RX2-Q). The mask uses bits 0..3 in that order. */
API_EXPORT
int CALL_CONV bladerf_set_rx_fpga_adc_enable_mask(struct bladerf *dev,
                                                   uint8_t mask);

/**
 * Latched summary of one completed dwell, from the sweep-revision spectrum
 * analyzer block. Fields are the fully-assembled physical/register values
 * -- the caller does not reassemble lo/hi word pairs.
 *
 * No unit conversion to physical quantities (dBFS etc.) is done here: the
 * library does not know the external RF chain, so these are raw
 * accumulator/register values as the fabric computed them.
 */
struct bladerf_dwell_summary {
    uint64_t energy_sum;      /**< Summed squared ADC magnitude over dwell */
    uint32_t peak;             /**< Largest single-sample I^2+Q^2 in the
                                 *   dwell. Power, not magnitude: full
                                 *   scale is 2048 per component, so this
                                 *   tops out at 2^23. */
    uint32_t clip_count;       /**< Samples with either component at or
                                 *   past the ADC rail */
    uint32_t sample_count;     /**< Samples included in this dwell */
    uint64_t first_timestamp;  /**< Sample timestamp at dwell start */
    uint32_t mean_power;       /**< Energy per WINDOW, not per sample:
                                 *   energy_sum >> WINDOW_LOG2. The fabric
                                 *   shifts rather than divides, because
                                 *   sample_count is not a power of two and
                                 *   a real divider is not worth the logic.
                                 *   Directly comparable with noise_floor
                                 *   and peak_window, which are also window
                                 *   sums. Divide by the window size for a
                                 *   per-sample figure. */
    uint64_t noise_floor;      /**< The QUIETEST completed window of the
                                 *   dwell, as a window sum. Not an
                                 *   estimate: with a signal present for
                                 *   part of the dwell, this is the part
                                 *   without it. Zero if no window
                                 *   completed -- check sample_count. */
    uint64_t peak_window;      /**< The LOUDEST completed window, same
                                 *   units. Against noise_floor it gives
                                 *   the dwell's dynamic range. */
    uint32_t first_window;     /**< Index of the window where the trigger
                                 *   first crossed. Meaningless unless
                                 *   verdict says it triggered. */
    uint32_t verdict;          /**< Advisory trigger: energy stayed over
                                 *   threshold for K of the last M windows.
                                 *   Advisory only -- the fabric never
                                 *   discards a dwell on it. */
    uint32_t generation;       /**< Generation this record belongs to; a
                                 *   device that has just reset may report
                                 *   generation 0 before any dwell
                                 *   completes */
};

/**
 * Drive the dwell boundary strobe (RFFE control register, SYNC_IN bit).
 *
 * A dwell boundary is not automatic: bladerf_core.vhd generates
 * dwell_start on the RISING EDGE of this bit (dwell_sync_in and not
 * dwell_sync_in_r), so the host must itself toggle it low->high->low
 * around each dwell it wants counted. RX must already be enabled and
 * streaming for the dwell to accumulate any samples.
 *
 * @param       dev     Device handle
 * @param[in]   value   New SYNC_IN level
 *
 * @return 0 on success, or a value from \ref RETCODES list on failure
 */
API_EXPORT
int CALL_CONV bladerf_set_dwell_sync(struct bladerf *dev, bool value);

/**
 * Read the dwell status word.
 *
 * Sweep-revision gateware only. The top 4 bits of the status word carry
 * the analyzer version marker (bladerf_core.vhd, dwell_status_word(31
 * downto 28)); hosted-revision gateware has the same FPGA version number
 * but does not implement this block, and reads back 0 in those bits. This
 * function checks that marker and returns BLADERF_ERR_UNSUPPORTED rather
 * than a silent zero when it is absent -- a zero status word is
 * indistinguishable from "nothing frozen, nothing triggered" otherwise.
 *
 * @param       dev     Device handle
 * @param[out]  value   Status word
 *
 * @return 0 on success, BLADERF_ERR_UNSUPPORTED if this gateware does not
 *         implement the dwell analyzer, or a value from \ref RETCODES
 *         list on other failure
 */
API_EXPORT
int CALL_CONV bladerf_get_dwell_status(struct bladerf *dev, uint32_t *value);

/**
 * Configure the dwell analyzer's advisory trigger threshold and settling
 * interval.
 *
 * @param       dev         Device handle
 * @param[in]   threshold   Window energy sum to trigger on, in raw summed
 *                          ADC units. Zero disables the trigger;
 *                          measurement continues either way -- this only
 *                          decides which dwells are flagged. Encoded as
 *                          mantissa and shift, so large values round to
 *                          about 0.2%.
 * @param[in]   settle_sel  Settling interval as a shift below the maximum:
 *                          0 = 8192 samples, 1 = 4096, 2 = 2048,
 *                          3 = 1024. At 61.44 MHz that spans 133 us down
 *                          to 17 us.
 *
 * @return 0 on success, BLADERF_ERR_UNSUPPORTED if this gateware does not
 *         implement the dwell analyzer, or a value from \ref RETCODES
 *         list on other failure
 */
API_EXPORT
int CALL_CONV bladerf_set_dwell_cfg(struct bladerf *dev, uint64_t threshold,
                                    uint8_t settle_sel);

/**
 * Read the latched dwell summary for the most recently completed dwell.
 *
 * Reads the generation marker before and after the record and retries
 * internally if a dwell boundary landed in between, so the fields
 * returned always describe a single dwell rather than a mix of two.
 *
 * @param       dev      Device handle
 * @param[out]  summary  Receives the assembled dwell summary
 *
 * @return 0 on success, BLADERF_ERR_UNSUPPORTED if this gateware does not
 *         implement the dwell analyzer, BLADERF_ERR_UNEXPECTED if the
 *         record could not be read consistently in four attempts, or a
 *         value from \ref RETCODES list on other failure
 */
API_EXPORT
int CALL_CONV bladerf_get_dwell_summary(struct bladerf *dev,
                                        struct bladerf_dwell_summary *summary);

/**
 * @defgroup FN_BLADERF2_RX_SPATIAL_FIR RX spatial FIR
 *
 * 2x2 complex 5-tap FIR between the AD9361 ADC lanes and the RX sample
 * path (doc/development/rx-spatial-mimo-fir-design.md):
 *
 *      y_i[n] = sum_j sum_k h_ij[k] * x_j[n-k],  k = 0..4, center tap 2
 *
 * Coefficients are Q2.14 (::BLADERF_RX_SPATIAL_FIR_ONE = 1.0), indexed
 * `coef[i][j][k][c]`: output lane i, input lane j, tap k, c = 0 real,
 * c = 1 imaginary. Outputs are rounded and saturated to 12 bits. The block
 * is a bit-exact pass-through (identity) whenever the bank is the identity,
 * either RX lane is disabled (RX_X1), or the identity level is forced.
 *
 * The fabric returns to identity on its own reset, which the FX3 pulses when
 * RX or TX is enabled while both directions were off: load W after
 * ::bladerf_enable_module, not before. libbladeRF also forces identity at
 * open, on every RX invalidation whose reason changes LO, gain, gain mode,
 * sample rate, bandwidth, RF port, correction, AD9361 FIR or clock, and at
 * the RX epoch fence of ::bladerf_rx_transition_begin (also for scheduled
 * retunes, at schedule time).
 *
 * @{
 */

#define BLADERF_RX_SPATIAL_FIR_TAPS   5
#define BLADERF_RX_SPATIAL_FIR_CENTER 2
#define BLADERF_RX_SPATIAL_FIR_ONE    16384 /**< 1.0 in Q2.14 */

/** State of the RX spatial FIR as read back from the FPGA. */
struct bladerf_rx_spatial_fir_status {
    /** Library counter, +1 per commit or identity reset by this handle. */
    uint32_t host_generation;
    /** Tag of the bank the fabric runs; 0 after a fabric reset. */
    uint8_t fpga_tag;
    /** Output is the unfiltered input right now (identity bank, RX_X1, or
     *  forced identity). */
    bool bypass_active;
    /** Active bank is the identity bank. */
    bool identity;
    /** Both RX lanes are enabled. */
    bool both_lanes;
    /** An RX1 / RX2 output saturated since the last commit. */
    bool sat_rx1, sat_rx2;
    /** RX epoch opened by the last commit of this handle, valid when the RX
     *  epoch contract was enabled at that commit; otherwise 0. */
    uint8_t rx_epoch_id;
    /** RX transaction of the last commit (for bladerf_rx_transition_wait /
     *  bladerf_rx_transition_get_events), same validity as rx_epoch_id. */
    uint32_t rx_transaction_id;
};

/**
 * Load a coefficient bank and make it active.
 *
 * With the RX epoch contract enabled (an earlier transition requested
 * BLADERF_RF_REQUIRE_EPOCH_VALID) the commit runs as an RX transaction: the
 * FPGA epoch is armed and fenced, the bank is committed, COMPLETE opens a
 * new epoch on the first sample whose 5-sample support follows the commit,
 * and the call returns after EPOCH_VALID. Without the contract the bank is
 * written and committed directly, as stock setters do. On any failure the
 * epoch is aborted and the block forced to identity.
 *
 * @param       dev   Device handle
 * @param[in]   coef  Q2.14 bank, see the group description
 *
 * @return 0 on success, BLADERF_ERR_UNSUPPORTED if the gateware lacks the
 *         block or an RX channel uses an AGC gain mode,
 *         BLADERF_ERR_NOT_INIT if the RX epoch contract is enabled and RX
 *         is disabled (the fenced commit needs RX samples; nothing is
 *         written), or a value from
 *         \ref RETCODES list on other failure
 */
API_EXPORT
int CALL_CONV bladerf_set_rx_spatial_fir(struct bladerf *dev,
                                         const int16_t coef[2][2][5][2]);

/**
 * Read the active bank and status.
 *
 * @param       dev   Device handle
 * @param[out]  coef  Active bank: identity when the FPGA reports an identity
 *                    bank, else the bank NIOS committed. May be NULL.
 * @param[out]  st    Status. May be NULL.
 *
 * @return 0 on success, BLADERF_ERR_UNSUPPORTED if the gateware lacks the
 *         block, BLADERF_ERR_UNEXPECTED if the FPGA tag does not match the
 *         last commit of this handle, or a value from \ref RETCODES list
 */
API_EXPORT
int CALL_CONV bladerf_get_rx_spatial_fir(struct bladerf *dev,
                                         int16_t coef[2][2][5][2],
                                         struct bladerf_rx_spatial_fir_status *st);

/**
 * Force identity (pass-through). Fenced like ::bladerf_set_rx_spatial_fir
 * when the RX epoch contract is enabled.
 *
 * @param       dev   Device handle
 *
 * @return 0 on success, BLADERF_ERR_UNSUPPORTED if the gateware lacks the
 *         block, BLADERF_ERR_NOT_INIT as for ::bladerf_set_rx_spatial_fir,
 *         or a value from \ref RETCODES list on other failure
 */
API_EXPORT
int CALL_CONV bladerf_reset_rx_spatial_fir(struct bladerf *dev);

/**
 * Read the RX spatial FIR status only.
 *
 * @param       dev   Device handle
 * @param[out]  st    Status
 *
 * @return 0 on success, BLADERF_ERR_UNSUPPORTED if the gateware lacks the
 *         block, or a value from \ref RETCODES list on other failure
 */
API_EXPORT
int CALL_CONV bladerf_get_rx_spatial_fir_status(
    struct bladerf *dev, struct bladerf_rx_spatial_fir_status *st);

/** @} (End of FN_BLADERF2_RX_SPATIAL_FIR) */

/**
 * Read the temperature from the RFIC
 *
 * @param       dev         Device handle
 * @param[out]  val         Temperature in degrees C
 *
 * @return 0 on success, value from \ref RETCODES list on failure
 */
API_EXPORT
int CALL_CONV bladerf_get_rfic_temperature(struct bladerf *dev, float *val);

/**
 * Read the RSSI for the selected channel from the RFIC
 *
 * @note  This is a relative value, not an absolute value. If an absolute
 *        value (e.g. in dBm) is desired, a calibration should be performed
 *        against a reference signal.
 *
 * @note  See `fpga_common/src/ad936x_params.c` for the RSSI control parameters.
 *
 * Reference: AD9361 Reference Manual UG-570
 *
 * @param       dev         Device handle
 * @param       ch          Channel to query
 * @param[out]  pre_rssi    Preamble RSSI in dB (first calculated RSSI result)
 * @param[out]  sym_rssi    Symbol RSSI in dB (most recent RSSI result)
 *
 * @return 0 on success, value from \ref RETCODES list on failure
 */
API_EXPORT
int CALL_CONV bladerf_get_rfic_rssi(struct bladerf *dev,
                                    bladerf_channel ch,
                                    int32_t *pre_rssi,
                                    int32_t *sym_rssi);

/**
 * Read the CTRL_OUT pins from the RFIC
 *
 * @note  See AD9361 Reference Manual UG-570's "Control Output" chapter for
 *        complete information about this feature.
 *
 * @see   bladerf_set_rfic_register()
 *
 * @param      dev       Device handle
 * @param[out] ctrl_out  Pointer for storing the retrieved value
 *
 * @return 0 on success, value from \ref RETCODES list on failure
 */
API_EXPORT
int CALL_CONV bladerf_get_rfic_ctrl_out(struct bladerf *dev, uint8_t *ctrl_out);

/**
 * RFIC RX FIR filter choices
 */
typedef enum {
    BLADERF_RFIC_RXFIR_BYPASS = 0, /**< No filter */
    BLADERF_RFIC_RXFIR_CUSTOM,     /**< Custom FIR filter (currently unused) */
    BLADERF_RFIC_RXFIR_DEC1,       /**< Decimate by 1 (default) */
    BLADERF_RFIC_RXFIR_DEC2,       /**< Decimate by 2 */
    BLADERF_RFIC_RXFIR_DEC4,       /**< Decimate by 4 */
} bladerf_rfic_rxfir;

/** Default RFIC RX FIR filter */
#define BLADERF_RFIC_RXFIR_DEFAULT BLADERF_RFIC_RXFIR_DEC1

/**
 * RFIC TX FIR filter choices
 */
typedef enum {
    BLADERF_RFIC_TXFIR_BYPASS = 0, /**< No filter (default) */
    BLADERF_RFIC_TXFIR_CUSTOM,     /**< Custom FIR filter (currently unused) */
    BLADERF_RFIC_TXFIR_INT1,       /**< Interpolate by 1 */
    BLADERF_RFIC_TXFIR_INT2,       /**< Interpolate by 2 */
    BLADERF_RFIC_TXFIR_INT4,       /**< Interpolate by 4 */
} bladerf_rfic_txfir;

/** Default RFIC TX FIR filter */
#define BLADERF_RFIC_TXFIR_DEFAULT BLADERF_RFIC_TXFIR_BYPASS

/**
 * Get the current status of the RX FIR filter on the RFIC.
 *
 * @param   dev     Device handle
 * @param   rxfir   RX FIR selection
 *
 * @note  See `fpga_common/src/ad936x_params.c` for FIR parameters.
 *
 * @return 0 on success, value from \ref RETCODES list on failure
 */
API_EXPORT
int CALL_CONV bladerf_get_rfic_rx_fir(struct bladerf *dev,
                                      bladerf_rfic_rxfir *rxfir);

/**
 * Set the RX FIR filter on the RFIC.
 *
 * @param   dev     Device handle
 * @param   rxfir   RX FIR selection
 *
 * @note  See `fpga_common/src/ad936x_params.c` for FIR parameters.
 *
 * @return 0 on success, value from \ref RETCODES list on failure
 */
API_EXPORT
int CALL_CONV bladerf_set_rfic_rx_fir(struct bladerf *dev,
                                      bladerf_rfic_rxfir rxfir);

/**
 * Get the current status of the TX FIR filter on the RFIC.
 *
 * @param   dev     Device handle
 * @param   txfir   TX FIR selection
 *
 * @note  See `fpga_common/src/ad936x_params.c` for FIR parameters.
 *
 * @return 0 on success, value from \ref RETCODES list on failure
 */
API_EXPORT
int CALL_CONV bladerf_get_rfic_tx_fir(struct bladerf *dev,
                                      bladerf_rfic_txfir *txfir);

/**
 * Set the TX FIR filter on the RFIC.
 *
 * @param   dev     Device handle
 * @param   txfir   TX FIR selection
 *
 * @note  See `fpga_common/src/ad936x_params.c` for FIR parameters.
 *
 * @return 0 on success, value from \ref RETCODES list on failure
 */
API_EXPORT
int CALL_CONV bladerf_set_rfic_tx_fir(struct bladerf *dev,
                                      bladerf_rfic_txfir txfir);

/** @} (End of FN_BLADERF2_LOW_LEVEL_RFIC) */

/**
 * @defgroup FN_BLADERF2_LOW_LEVEL_PLL Phase Detector/Freq. Synth Control
 *
 * Reference:
 * http://www.analog.com/media/en/technical-documentation/data-sheets/ADF4002.pdf
 *
 * @{
 */

/**
 * Fetch the lock state of the Phase Detector/Frequency Synthesizer
 *
 * @param       dev         Device handle
 * @param[out]  locked      True if locked, False otherwise
 *
 * @return 0 on success, value from \ref RETCODES list on failure
 */
API_EXPORT
int CALL_CONV bladerf_get_pll_lock_state(struct bladerf *dev, bool *locked);

/**
 * Fetch the state of the Phase Detector/Frequency Synthesizer
 *
 * @param       dev         Device handle
 * @param[out]  enabled     True if enabled, False otherwise
 *
 * @return 0 on success, value from \ref RETCODES list on failure
 */
API_EXPORT
int CALL_CONV bladerf_get_pll_enable(struct bladerf *dev, bool *enabled);

/**
 * Enable the Phase Detector/Frequency Synthesizer
 *
 * Enabling this disables the VCTCXO trimmer DAC, and vice versa.
 *
 * @param       dev         Device handle
 * @param[in]   enable      True to enable, False otherwise
 *
 * @return 0 on success, value from \ref RETCODES list on failure
 */
API_EXPORT
int CALL_CONV bladerf_set_pll_enable(struct bladerf *dev, bool enable);

/**
 * Get the valid range of frequencies for the reference clock input
 *
 * @param       dev         Device handle
 * @param[out]  range       Reference clock frequency range
 *
 * @return 0 on success, value from \ref RETCODES list on failure
 */
API_EXPORT
int CALL_CONV bladerf_get_pll_refclk_range(struct bladerf *dev,
                                           const struct bladerf_range **range);

/**
 * Get the currently-configured frequency for the reference clock
 * input.
 *
 * @param       dev         Device handle
 * @param[out]  frequency   Reference clock frequency
 *
 * @return 0 on success, value from \ref RETCODES list on failure
 */
API_EXPORT
int CALL_CONV bladerf_get_pll_refclk(struct bladerf *dev, uint64_t *frequency);

/**
 * Set the expected frequency for the reference clock input.
 *
 * @param       dev         Device handle
 * @param[in]   frequency   Reference clock frequency
 *
 * @return 0 on success, value from \ref RETCODES list on failure
 */
API_EXPORT
int CALL_CONV bladerf_set_pll_refclk(struct bladerf *dev, uint64_t frequency);

/**
 * Read value from Phase Detector/Frequency Synthesizer
 *
 * The `address` is interpreted as the control bits (DB1 and DB0) used to write
 * to a specific latch.
 *
 * @param       dev         Device handle
 * @param[in]   address     Latch address
 * @param[out]  val         Value to read from
 *
 * @return 0 on success, value from \ref RETCODES list on failure
 */
API_EXPORT
int CALL_CONV bladerf_get_pll_register(struct bladerf *dev,
                                       uint8_t address,
                                       uint32_t *val);

/**
 * Write value to Phase Detector/Frequency Synthesizer
 *
 * The `address` is interpreted as the control bits (DB1 and DB0) used to write
 * to a specific latch.  These bits are masked out in `val`
 *
 * @param       dev         Device handle
 * @param[in]   address     Latch address
 * @param[in]   val         Value to write to
 *
 * @return 0 on success, value from \ref RETCODES list on failure
 */
API_EXPORT
int CALL_CONV bladerf_set_pll_register(struct bladerf *dev,
                                       uint8_t address,
                                       uint32_t val);

/** @} (End of FN_BLADERF2_LOW_LEVEL_PLL) */

/**
 * @defgroup FN_BLADERF2_LOW_LEVEL_POWER_SOURCE Power Multiplexer
 *
 * @{
 */

/**
 * Power sources
 */
typedef enum {
    BLADERF_UNKNOWN,    /**< Unknown; manual observation may be required */
    BLADERF_PS_DC,      /**< DC Barrel Plug */
    BLADERF_PS_USB_VBUS /**< USB Bus */
} bladerf_power_sources;

/**
 * Get the active power source reported by the power multiplexer
 *
 * Reference: http://www.ti.com/product/TPS2115A
 *
 * @param       dev     Device handle
 * @param[out]  val     Value read from power multiplexer
 *
 * @return 0 on success, value from \ref RETCODES list on failure
 */
API_EXPORT
int CALL_CONV bladerf_get_power_source(struct bladerf *dev,
                                       bladerf_power_sources *val);

/** @} (End of FN_BLADERF2_LOW_LEVEL_POWER_SOURCE) */

/**
 * @defgroup FN_BLADERF2_LOW_LEVEL_CLOCK_BUFFER Clock Buffer
 *
 * @{
 */

/**
 * @defgroup FN_BLADERF2_LOW_LEVEL_CLOCK_BUFFER_SELECT Clock input selection
 *
 * @{
 */

/**
 * Available clock sources
 */
typedef enum {
    CLOCK_SELECT_ONBOARD, /**< Use onboard VCTCXO */
    CLOCK_SELECT_EXTERNAL /**< Use external clock input */
} bladerf_clock_select;

/**
 * Get the selected clock source
 *
 * Reference: https://www.silabs.com/documents/public/data-sheets/Si53304.pdf
 *
 * @param       dev     Device handle
 * @param[out]  sel     Clock input source currently in use
 *
 * @return 0 on success, value from \ref RETCODES list on failure
 */
API_EXPORT
int CALL_CONV bladerf_get_clock_select(struct bladerf *dev,
                                       bladerf_clock_select *sel);

/**
 * Set the clock source
 *
 * Reference: https://www.silabs.com/documents/public/data-sheets/Si53304.pdf
 *
 * @param       dev     Device handle
 * @param[in]   sel     Clock input source to use
 *
 * @return 0 on success, value from \ref RETCODES list on failure
 */
API_EXPORT
int CALL_CONV bladerf_set_clock_select(struct bladerf *dev,
                                       bladerf_clock_select sel);

/** @} (End of FN_BLADERF2_LOW_LEVEL_CLOCK_BUFFER_SELECT) */

/**
 * @defgroup FN_BLADERF2_LOW_LEVEL_CLOCK_BUFFER_OUTPUT Clock output control
 *
 * @{
 */

/**
 * Get the current state of the clock output
 *
 * @param       dev     Device handle
 * @param[out]  state   Clock output state
 *
 * @return 0 on success, value from \ref RETCODES list on failure
 */
API_EXPORT
int CALL_CONV bladerf_get_clock_output(struct bladerf *dev, bool *state);

/**
 * Set the clock output (enable/disable)
 *
 * @param       dev     Device handle
 * @param[in]   enable  Clock output enable
 *
 * @return 0 on success, value from \ref RETCODES list on failure
 */
API_EXPORT
int CALL_CONV bladerf_set_clock_output(struct bladerf *dev, bool enable);

/** @} (End of FN_BLADERF2_LOW_LEVEL_CLOCK_BUFFER_OUTPUT) */

/** @} (End of FN_BLADERF2_LOW_LEVEL_CLOCK_BUFFER) */

/**
 * @defgroup FN_BLADERF2_LOW_LEVEL_PMIC Power Monitoring
 *
 * @{
 */

/**
 * Register identifiers for PMIC
 */
typedef enum {
    BLADERF_PMIC_CONFIGURATION, /**< Configuration register (uint16_t) */
    BLADERF_PMIC_VOLTAGE_SHUNT, /**< Shunt voltage (float) */
    BLADERF_PMIC_VOLTAGE_BUS,   /**< Bus voltage (float) */
    BLADERF_PMIC_POWER,         /**< Load power (float) */
    BLADERF_PMIC_CURRENT,       /**< Load current (float) */
    BLADERF_PMIC_CALIBRATION,   /**< Calibration (uint16_t) */
} bladerf_pmic_register;

/**
 * Read value from Power Monitor IC
 *
 * Reference: http://www.ti.com/product/INA219
 *
 * @param       dev     Device handle
 * @param[in]   reg     Register to read from
 * @param[out]  val     Value read from PMIC
 *
 * @return 0 on success, value from \ref RETCODES list on failure
 */
API_EXPORT
int CALL_CONV bladerf_get_pmic_register(struct bladerf *dev,
                                        bladerf_pmic_register reg,
                                        void *val);

/** @} (End of FN_BLADERF2_LOW_LEVEL_PMIC) */

/**
 * @defgroup FN_BLADERF2_LOW_LEVEL_RF_SWITCHING RF Switching Control
 *
 * @{
 */

/**
 * RF switch configuration structure
 */
typedef struct {
    uint32_t tx1_rfic_port; /**< Active TX1 output from RFIC */
    uint32_t tx1_spdt_port; /**< RF switch configuration for the TX1 path */
    uint32_t tx2_rfic_port; /**< Active TX2 output from RFIC */
    uint32_t tx2_spdt_port; /**< RF switch configuration for the TX2 path */
    uint32_t rx1_rfic_port; /**< Active RX1 input to RFIC */
    uint32_t rx1_spdt_port; /**< RF switch configuration for the RX1 path */
    uint32_t rx2_rfic_port; /**< Active RX2 input to RFIC */
    uint32_t rx2_spdt_port; /**< RF switch configuration for the RX2 path */
} bladerf_rf_switch_config;

/**
 * Read the current RF switching configuration from the bladeRF hardware.
 *
 * Queries both the RFIC and the RF switch and passes back a
 * bladerf_rf_switch_config stucture.
 *
 * @param       dev     Device handle
 * @param[out]  config  Switch configuration struct
 *
 * @return 0 on success, value from \ref RETCODES list on failure
 */
API_EXPORT
int CALL_CONV bladerf_get_rf_switch_config(struct bladerf *dev,
                                           bladerf_rf_switch_config *config);

/** @} (End of FN_BLADERF2_LOW_LEVEL_RF_SWITCHING) */

/**
 * @defgroup FN_BLADERF2_LOW_LEVEL_NIOS_RAM NIOS RAM usage
 *
 * The NIOS II stack and heap share the RAM between the end of the firmware's
 * static data and the top of on-chip memory. The firmware fills that gap
 * with a pattern at boot; any word no longer holding it has been written.
 *
 * @{
 */

/**
 * NIOS stack/heap usage since the last FPGA load, in bytes.
 *
 * untouched + heap + stack == region.
 */
struct bladerf_nios_ram_usage {
    /** Gap bytes neither stack nor heap has written since boot: the
     *  remaining margin */
    uint32_t untouched;
    /** Heap extent; the allocator never returns memory, so this is its
     *  peak */
    uint32_t heap;
    /** Deepest stack since boot. A frame that reserves words it never
     *  writes is not seen. */
    uint32_t stack;
    /** RAM shared by the stack and the heap */
    uint32_t region;
};

/**
 * Read the NIOS stack/heap high-water mark.
 *
 * Each field is one NIOS request; the set is retried until it adds up, so
 * a stack that deepened between requests does not produce a torn result.
 *
 * @param       dev     Device handle
 * @param[out]  usage   Byte counts
 *
 * @return 0 on success, value from \ref RETCODES list on failure.
 *         Returns ::BLADERF_ERR_FPGA_OP on FPGA images that predate the
 *         read, ::BLADERF_ERR_WOULD_BLOCK if no consistent set was read
 *         after bounded retries.
 */
API_EXPORT
int CALL_CONV bladerf_get_nios_ram_usage(struct bladerf *dev,
                                         struct bladerf_nios_ram_usage *usage);

/** @} (End of FN_BLADERF2_LOW_LEVEL_NIOS_RAM) */

/**
 * @defgroup FN_BLADERF2_LOW_LEVEL_NIOS_I2C NIOS I2C controller state
 *
 * State of the OpenCores I2C controller that NIOS uses for the INA219 power
 * monitor, and its last failure. Readable while an INA219 access fails.
 *
 * @{
 */

/**
 * NIOS I2C diagnostic words; bit layouts are documented with
 * NIOS_PKT_8x32_TARGET_I2C_DIAG in nios_pkt_8x32.h.
 */
struct bladerf_nios_i2c_diag {
    /** [7:0] PRER lo, [15:8] PRER hi, [23:16] CTRL, [31:24] SR */
    uint32_t regs;
    /** [7:0] CR readback, [15:8] {SDA, SCL, slave_wait, byte state} */
    uint32_t core;
    /** Last failure: [7:0] operation, [15:8] byte step, [23:16] reason,
     *  [31:24] SR then; 0 if none since the FPGA load */
    uint32_t failure;
    /** Failed transfers since the FPGA load */
    uint32_t failures;
    /** [15:0] controller re-initialisations, [23:16] flags of the last,
     *  [31:24] diagnostic byte after it */
    uint32_t recovery;
};

/**
 * Read the NIOS I2C controller state and failure record.
 *
 * @param       dev     Device handle
 * @param[out]  diag    Diagnostic words
 *
 * @return 0 on success, value from \ref RETCODES list on failure.
 *         Returns ::BLADERF_ERR_FPGA_OP on FPGA images that predate the
 *         read.
 */
API_EXPORT
int CALL_CONV bladerf_get_nios_i2c_diag(struct bladerf *dev,
                                        struct bladerf_nios_i2c_diag *diag);

/**
 * Path of the FPGA image bladerf_open() loads into an unconfigured FPGA:
 * the first hostedx<size>.rbf for this board in the libbladeRF search path.
 *
 * Usable on a handle opened without FPGA initialization
 * (BLADERF_FORCE_NO_FPGA_PRESENT), so a caller can reload the same image
 * with bladerf_load_fpga() when initialization of the running one fails.
 *
 * @param       dev     Device handle
 * @param[out]  path    Buffer for the NUL-terminated path
 * @param[in]   len     Buffer size
 *
 * @return 0 on success, ::BLADERF_ERR_NO_FILE if no image is found,
 *         ::BLADERF_ERR_INVAL if the buffer is too small, other value from
 *         \ref RETCODES on failure.
 */
API_EXPORT
int CALL_CONV bladerf_get_fpga_image_path(struct bladerf *dev,
                                          char *path,
                                          size_t len);

/** @} (End of FN_BLADERF2_LOW_LEVEL_NIOS_I2C) */

/** @} (End of FN_BLADERF2_LOW_LEVEL) */

/** @} (End of BLADERF2) */

#endif /* BLADERF2_H_ */
